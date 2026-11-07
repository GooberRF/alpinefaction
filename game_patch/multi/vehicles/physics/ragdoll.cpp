#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include "LinearMath/btTransformUtil.h"
#include "ragdoll.h"
#include "vphys_internal.h"
#include "../../../misc/alpine_settings.h"
#include "../../../misc/settings_registry.h"
#include "../../../os/console.h"
#include "../../../os/os.h"
#include "../../../rf/character.h"
#include "../../../rf/corpse.h"
#include "../../../rf/entity.h"
#include "../../../rf/multi.h"
#include "../../../rf/physics.h"
#include "../../../rf/player/player.h"
#include "../../../rf/save_restore.h"
#include "../../../rf/v3d.h"
#include "../../../rf/vmesh.h"
#include "../../../rf/weapon.h"

namespace
{
    constexpr int ragdoll_max_bones = 50;
    constexpr int ragdoll_max_bodies = 24;
    constexpr float ragdoll_weld_reach = 0.16f;
    // Keeps a leaf bone with real skin, such as the head, whose reach is 0.
    constexpr float ragdoll_skin_body_share = 0.04f;
    constexpr int ragdoll_min_fit_verts = 6;
    constexpr float ragdoll_min_axis_length = 0.06f;
    // Keeps a mesh with stray coordinates from pairing a capsule with half the level.
    constexpr float ragdoll_max_model_extent = 64.0f;
    constexpr float ragdoll_max_world_extent = 100000.0f;
    constexpr float ragdoll_max_capsule_length = 6.0f;
    constexpr float ragdoll_max_scale_error = 0.1f;
    constexpr float ragdoll_min_radius = 0.035f;
    constexpr float ragdoll_max_radius = 0.3f;
    constexpr float ragdoll_density = 1000.0f;
    constexpr float ragdoll_min_mass = 2.0f;
    constexpr float ragdoll_max_mass = 18.0f;

    constexpr int ragdoll_min_joint_samples = 24;
    constexpr size_t ragdoll_max_joint_samples = 2048;
    constexpr int ragdoll_mean_iterations = 4;
    constexpr float ragdoll_range_trim = 0.02f;
    constexpr float ragdoll_default_half_range = 0.8f;
    constexpr float ragdoll_min_half_range = 0.06f;
    // btGeneric6DofConstraint's Euler form is singular at +-90 degrees on Y.
    constexpr float ragdoll_max_half_range_y = 1.45f;
    constexpr float ragdoll_max_half_range = 3.1f;
    constexpr float ragdoll_min_track_weight = 0.1f;
    constexpr int ragdoll_max_keys_per_anim = 64;

    constexpr int64_t ragdoll_hit_window_ms = 400;
    constexpr float ragdoll_min_blast_radius = 0.5f;
    constexpr float ragdoll_blast_reach = 1.5f;
    constexpr float ragdoll_room_update_dist = 0.25f;
    constexpr float ragdoll_min_sample_dt = 0.001f;
    constexpr float ragdoll_min_weapon_path = 0.01f;
    constexpr float ragdoll_blast_surface_lift = 0.1f;
    constexpr float ragdoll_max_blood_damage = 100.0f;
    constexpr float ragdoll_hit_kick_damage = 40.0f;
    constexpr float ragdoll_min_hit_kick_scale = 0.5f;
    constexpr float ragdoll_max_hit_kick_scale = 2.5f;
    constexpr float ragdoll_floor_clearance = 0.1f;

    constexpr float ragdoll_delay_ms = 150.0f;
    constexpr float ragdoll_big_hit_damage = 60.0f;
    constexpr float ragdoll_hit_kick = 1.0f;
    constexpr float ragdoll_hit_kick_share = 0.35f;
    constexpr float ragdoll_blast_kick = 11.0f;
    constexpr float ragdoll_blast_lift = 0.35f;
    constexpr float ragdoll_killer_kick = 2.0f;
    constexpr float ragdoll_limit_pad_deg = 12.0f;
    constexpr float ragdoll_friction = 0.75f;
    constexpr float ragdoll_restitution = 0.02f;
    constexpr float ragdoll_linear_damping = 0.04f;
    constexpr float ragdoll_angular_damping = 0.3f;
    constexpr float ragdoll_max_anim_speed = 8.0f;
    constexpr float ragdoll_max_anim_spin = 20.0f;
    constexpr float ragdoll_max_entity_speed = 30.0f;
    constexpr float ragdoll_freeze_speed = 0.3f;
    constexpr float ragdoll_freeze_spin = 1.2f;
    constexpr float ragdoll_freeze_seconds = 0.6f;
    constexpr float ragdoll_max_seconds = 15.0f;

    struct RagdollRigBody
    {
        int bone = -1;
        int parent_body = -1;
        int parent_bone = -1; // may be welded into parent_body
        std::unique_ptr<btCapsuleShape> shape;
        float mass = 1.0f;
        btVector3 inertia{0.0f, 0.0f, 0.0f};
        btTransform body_in_bone = btTransform::getIdentity();
        btTransform bone_in_body = btTransform::getIdentity();
        // X and Z are the widest axes, keeping Y off its Euler singularity.
        btQuaternion joint_in_parent_bone = btQuaternion::getIdentity();
        btQuaternion joint_in_bone = btQuaternion::getIdentity();
        btVector3 half_range{ragdoll_default_half_range, ragdoll_default_half_range, ragdoll_default_half_range};
    };

    struct RagdollRig
    {
        int num_bones = 0;
        float bound_radius = 0.0f; // around the root bone
        std::array<int, ragdoll_max_bones> body_of{};
        std::vector<RagdollRigBody> bodies; // parents first
    };

    enum class RagdollPhase
    {
        sampling,
        simulating,
        frozen,
    };

    struct RagdollPart
    {
        std::unique_ptr<btDefaultMotionState> motion;
        std::unique_ptr<btRigidBody> body;
        std::unique_ptr<btGeneric6DofConstraint> joint;
    };

    struct Ragdoll
    {
        rf::CharacterInstance* ci = nullptr;
        const RagdollRig* rig = nullptr;
        int entity_handle = -1;
        int last_weapon_handle = -1;
        uint8_t killer_id = 0xFF;
        int owner_handle = -1;
        bool owner_is_corpse = false;
        bool evicted = false; // its corpse is flagged dead; the entry leaves with the instance
        RagdollPhase phase = RagdollPhase::sampling;
        int samples = 0;
        float age = 0.0f;
        float sample_dt = 0.0f;
        float still_time = 0.0f;
        float sim_time = 0.0f;
        rf::Vector3 room_pos{};
        std::array<rf::Matrix43, ragdoll_max_bones> prev_pose{};
        std::array<rf::Matrix43, ragdoll_max_bones> pose{};
        std::array<btTransform, ragdoll_max_bones> bone_in_head{};
        std::array<btTransform, ragdoll_max_bodies> head_world{};
        std::array<RagdollPart, ragdoll_max_bodies> parts{};
    };

    struct RagdollHit
    {
        int entity_handle = -1;
        rf::Vector3 point{};
        rf::Vector3 dir{};
        float damage = 0.0f;
        int64_t time_ms = 0;
    };

    struct RagdollBlast
    {
        rf::Vector3 pos{};
        float radius = 0.0f;
        int64_t time_ms = 0;
    };

    std::vector<std::unique_ptr<Ragdoll>> g_ragdolls; // oldest first
    std::unordered_map<const rf::CharacterInstance*, Ragdoll*> g_ragdoll_by_ci;
    std::unordered_map<const rf::Character*, std::unique_ptr<RagdollRig>> g_ragdoll_rigs; // null: no rig
    std::array<RagdollHit, 32> g_ragdoll_hits{};
    std::array<RagdollBlast, 16> g_ragdoll_blasts{};
    size_t g_ragdoll_next_hit = 0;
    size_t g_ragdoll_next_blast = 0;

    float weapon_damage(const rf::Weapon& wp)
    {
        return rf::is_multi ? wp.info->damage_multi : wp.info->damage;
    }

    btTransform to_transform(const rf::Matrix43& m)
    {
        return btTransform(to_bt(m.orient), to_bt(m.origin));
    }

    btTransform object_transform(const rf::Object& obj)
    {
        return btTransform(to_bt(obj.orient), to_bt(obj.pos));
    }

    btVector3 clamped(const btVector3& v, float max_len)
    {
        const float len = v.length();
        if (!std::isfinite(len)) {
            return btVector3{0.0f, 0.0f, 0.0f};
        }
        return len > max_len ? v * (max_len / len) : v;
    }

    // False for NaN. getRotation is only defined on an unscaled basis.
    bool is_rigid(const btTransform& t, float max_extent)
    {
        return std::abs(t.getBasis().determinant() - 1.0f) < ragdoll_max_scale_error
            && t.getOrigin().length2() < max_extent * max_extent;
    }

    float percentile(std::vector<float>& values, float fraction)
    {
        const size_t index = static_cast<size_t>(fraction * static_cast<float>(values.size() - 1));
        std::nth_element(values.begin(), values.begin() + index, values.end());
        return values[index];
    }

    // ─── Rig ─────────────────────────────────────────────────────────────────

    btVector3 quat_log(const btQuaternion& q)
    {
        const float sign = q.w() < 0.0f ? -1.0f : 1.0f;
        const btVector3 v{q.x() * sign, q.y() * sign, q.z() * sign};
        const float s = v.length();
        return s < 1.0e-6f ? v * 2.0f : v * (2.0f * std::atan2(s, q.w() * sign) / s);
    }

    btQuaternion quat_exp(const btVector3& w)
    {
        const float angle = w.length();
        return angle < 1.0e-6f ? btQuaternion::getIdentity() : btQuaternion(w / angle, angle);
    }

    using RagdollSkin = std::array<std::vector<btVector3>, ragdoll_max_bones>;

    // A vertex with a same_vertex_offset has no position of its own.
    int gather_skin(const rf::Character& ch, RagdollSkin& skin)
    {
        if (ch.num_character_meshes < 1) {
            return 0;
        }
        const rf::V3dMesh* mesh = ch.character_meshes[0].mesh;
        if (!mesh || !mesh->vu || mesh->vu->num_levels < 1 || !mesh->vu->meshes[0]) {
            return 0;
        }
        int total = 0;
        const rf::VifMesh& vif = *mesh->vu->meshes[0];
        for (int c = 0; c < vif.num_chunks; ++c) {
            const rf::VifChunk& chunk = vif.chunks[c];
            if (!chunk.vecs || !chunk.wi) {
                continue;
            }
            for (int v = 0; v < chunk.num_vecs; ++v) {
                if (chunk.same_vertex_offsets && chunk.same_vertex_offsets[v] > 0) {
                    continue;
                }
                const rf::WeightIndexArray& wi = chunk.wi[v];
                const int heaviest = static_cast<int>(std::max_element(wi.weights, wi.weights + 4) - wi.weights);
                const int bone = wi.indices[heaviest];
                const btVector3 pos = to_bt(chunk.vecs[v]);
                if (wi.weights[heaviest] > 0 && bone < ch.num_bones
                    && pos.length2() < ragdoll_max_model_extent * ragdoll_max_model_extent) {
                    skin[bone].push_back(pos);
                    ++total;
                }
            }
        }
        return total;
    }

    void fit_capsule(RagdollRigBody& body, const btTransform& bind, const btVector3& tip,
                     const std::vector<btVector3>& verts)
    {
        const btVector3 pivot = bind.getOrigin();
        btVector3 reach = tip - pivot;
        float reach_len = reach.length();
        if (reach_len < ragdoll_min_axis_length) {
            btVector3 centroid{0.0f, 0.0f, 0.0f};
            for (const btVector3& v : verts) {
                centroid += v;
            }
            reach = verts.empty() ? btVector3(0.0f, 1.0f, 0.0f) : centroid / static_cast<float>(verts.size()) - pivot;
            reach_len = 0.0f;
        }
        const btVector3 axis = reach.safeNormalize();

        float t_min = 0.0f;
        float t_max = reach_len;
        float radius = reach_len * 0.3f;
        if (static_cast<int>(verts.size()) >= ragdoll_min_fit_verts) {
            std::vector<float> radial;
            radial.reserve(verts.size());
            for (const btVector3& v : verts) {
                const btVector3 offset = v - pivot;
                const float t = offset.dot(axis);
                t_min = std::min(t_min, t);
                t_max = std::max(t_max, t);
                radial.push_back((offset - axis * t).length());
            }
            radius = percentile(radial, 0.8f);
        }
        radius = std::clamp(radius, ragdoll_min_radius, ragdoll_max_radius);
        const float length = std::min(t_max - t_min, ragdoll_max_capsule_length);
        const float cylinder = std::max(length - radius * 2.0f, 0.01f);

        const btVector3 centre = pivot + axis * ((t_min + t_max) * 0.5f);
        const btTransform body_in_model(shortestArcQuat(btVector3(0.0f, 1.0f, 0.0f), axis), centre);
        body.body_in_bone = bind.inverse() * body_in_model;
        body.bone_in_body = body.body_in_bone.inverse();
        body.shape = std::make_unique<btCapsuleShape>(radius, cylinder);
        const float volume = SIMD_PI * radius * radius * (cylinder + radius * 4.0f / 3.0f);
        body.mass = std::clamp(volume * ragdoll_density, ragdoll_min_mass, ragdoll_max_mass);
        body.shape->calculateLocalInertia(body.mass, body.inertia);
    }

    // RF matrices hold the key's rows, so in Bullet's column form the key is the conjugate.
    std::vector<btQuaternion> joint_samples(const rf::Character& ch, int bone)
    {
        std::vector<btQuaternion> samples;
        for (int a = 0; a < ch.num_anims && a < static_cast<int>(std::size(ch.animations)); ++a) {
            const rf::Skeleton* skeleton = ch.animations[a];
            if (!skeleton || !skeleton->animation_data) {
                continue;
            }
            const auto* anim = static_cast<const rf::SkeletonAnimation*>(skeleton->animation_data);
            const size_t size = skeleton->data_size > 0 ? static_cast<size_t>(skeleton->data_size) : 0;
            const size_t table_end = offsetof(rf::SkeletonAnimation, bone_offsets) + sizeof(int) * ch.num_bones;
            if (size < table_end || anim->num_bones != ch.num_bones || anim->bone_offsets[bone] <= 0) {
                continue;
            }
            const size_t offset = static_cast<size_t>(anim->bone_offsets[bone]);
            const size_t keys_begin = offset + offsetof(rf::SkeletonBoneTrack, rotation_keys);
            if (keys_begin > size) {
                continue;
            }
            const auto* track = reinterpret_cast<const rf::SkeletonBoneTrack*>(
                static_cast<const char*>(skeleton->animation_data) + offset);
            const int num_keys = track->num_rotation_keys;
            if (track->weight < ragdoll_min_track_weight || num_keys <= 0
                || keys_begin + sizeof(rf::SkeletonRotationKey) * num_keys > size) {
                continue;
            }
            const int key_stride = num_keys / ragdoll_max_keys_per_anim + 1;
            for (int k = 0; k < num_keys; k += key_stride) {
                const short* r = track->rotation_keys[k].rotation;
                const btQuaternion q{-static_cast<float>(r[0]), -static_cast<float>(r[1]),
                                     -static_cast<float>(r[2]), static_cast<float>(r[3])};
                if (q.length2() > 1.0f) {
                    samples.push_back(q.normalized());
                }
            }
        }
        if (samples.size() > ragdoll_max_joint_samples) {
            const size_t stride = samples.size() / ragdoll_max_joint_samples + 1;
            size_t kept = 0;
            for (size_t i = 0; i < samples.size(); i += stride) {
                samples[kept++] = samples[i];
            }
            samples.resize(kept);
        }
        return samples;
    }

    bool fit_joint(RagdollRigBody& body, const std::vector<btQuaternion>& samples)
    {
        if (static_cast<int>(samples.size()) < ragdoll_min_joint_samples) {
            return false;
        }
        btQuaternion mean = samples[0];
        for (int i = 0; i < ragdoll_mean_iterations; ++i) {
            btVector3 sum{0.0f, 0.0f, 0.0f};
            const btQuaternion inverse = mean.inverse();
            for (const btQuaternion& q : samples) {
                sum += quat_log(inverse * q);
            }
            mean = (mean * quat_exp(sum / static_cast<float>(samples.size()))).normalized();
        }

        const float count = static_cast<float>(samples.size());
        std::vector<btVector3> spread;
        spread.reserve(samples.size());
        btVector3 centre{0.0f, 0.0f, 0.0f};
        const btQuaternion inverse = mean.inverse();
        for (const btQuaternion& q : samples) {
            spread.push_back(quat_log(inverse * q));
            centre += spread.back();
        }
        centre /= count;
        btScalar xx = 0.0f, xy = 0.0f, xz = 0.0f, yy = 0.0f, yz = 0.0f, zz = 0.0f;
        for (const btVector3& s : spread) {
            const btVector3 d = s - centre;
            xx += d.x() * d.x();
            xy += d.x() * d.y();
            xz += d.x() * d.z();
            yy += d.y() * d.y();
            yz += d.y() * d.z();
            zz += d.z() * d.z();
        }
        btMatrix3x3 covariance{xx, xy, xz, xy, yy, yz, xz, yz, zz};
        btMatrix3x3 eigenvectors;
        covariance.diagonalize(eigenvectors, 1.0e-6f, 32);

        struct Axis
        {
            btVector3 dir;
            float low;
            float high;
        };
        std::array<Axis, 3> axes;
        std::vector<float> projected(spread.size());
        for (int a = 0; a < 3; ++a) {
            const btVector3 dir = eigenvectors.getColumn(a).safeNormalize();
            for (size_t i = 0; i < spread.size(); ++i) {
                projected[i] = spread[i].dot(dir);
            }
            const float low = percentile(projected, ragdoll_range_trim);
            const float high = percentile(projected, 1.0f - ragdoll_range_trim);
            axes[a] = Axis{dir, low, high};
        }
        std::sort(axes.begin(), axes.end(),
                  [](const Axis& a, const Axis& b) { return a.high - a.low > b.high - b.low; });

        btVector3 middle{0.0f, 0.0f, 0.0f};
        for (const Axis& axis : axes) {
            middle += axis.dir * ((axis.low + axis.high) * 0.5f);
        }
        const btVector3 x_axis = axes[0].dir;
        const btVector3 z_axis = axes[1].dir;
        const btVector3 y_axis = z_axis.cross(x_axis).safeNormalize();
        const btMatrix3x3 frame{x_axis.x(), y_axis.x(), z_axis.x(),
                                x_axis.y(), y_axis.y(), z_axis.y(),
                                x_axis.z(), y_axis.z(), z_axis.z()};
        btQuaternion frame_rotation;
        frame.getRotation(frame_rotation);
        body.joint_in_bone = frame_rotation;
        body.joint_in_parent_bone = (mean * quat_exp(middle)).normalized() * frame_rotation;
        body.half_range = btVector3{(axes[0].high - axes[0].low) * 0.5f, (axes[2].high - axes[2].low) * 0.5f,
                                    (axes[1].high - axes[1].low) * 0.5f};
        return true;
    }

    std::unique_ptr<RagdollRig> build_rig(const rf::Character& ch)
    {
        const int num_bones = ch.num_bones;
        if (num_bones < 2 || num_bones > ragdoll_max_bones) {
            return nullptr;
        }
        std::array<btTransform, ragdoll_max_bones> bind;
        std::array<int, ragdoll_max_bones> parent{};
        int root = -1;
        for (int i = 0; i < num_bones; ++i) {
            parent[i] = ch.bones[i].parent;
            bind[i] = to_transform(ch.bones[i].transform).inverse();
            if (parent[i] >= num_bones || !is_rigid(bind[i], ragdoll_max_model_extent)) {
                return nullptr;
            }
            if (parent[i] < 0) {
                if (root >= 0) {
                    return nullptr;
                }
                root = i;
            }
        }
        std::array<bool, ragdoll_max_bones> seen{};
        for (int k = 0; k < num_bones; ++k) {
            const int bone = ch.bone_order_list[k];
            if (bone >= num_bones || seen[bone] || (parent[bone] >= 0 && !seen[parent[bone]])) {
                return nullptr;
            }
            seen[bone] = true;
        }
        if (root < 0) {
            return nullptr;
        }

        std::array<float, ragdoll_max_bones> reach{};
        std::array<btVector3, ragdoll_max_bones> child_sum;
        std::array<int, ragdoll_max_bones> child_count{};
        child_sum.fill(btVector3{0.0f, 0.0f, 0.0f});
        for (int i = 0; i < num_bones; ++i) {
            if (parent[i] >= 0) {
                child_sum[parent[i]] += bind[i].getOrigin();
                ++child_count[parent[i]];
            }
            for (int a = parent[i]; a >= 0; a = parent[a]) {
                reach[a] = std::max(reach[a], (bind[i].getOrigin() - bind[a].getOrigin()).length());
            }
        }

        auto skin = std::make_unique<RagdollSkin>();
        const int total_verts = gather_skin(ch, *skin);
        std::array<bool, ragdoll_max_bones> is_body{};
        for (int i = 0; i < num_bones; ++i) {
            const float share = total_verts > 0 ? static_cast<float>((*skin)[i].size()) / total_verts : 0.0f;
            is_body[i] = reach[i] >= ragdoll_weld_reach || share >= ragdoll_skin_body_share;
        }
        is_body[root] = true;
        // A root usually has no skin of its own, so it shares a body with its busiest child.
        int torso = -1;
        for (int i = 0; i < num_bones; ++i) {
            if (parent[i] == root && (torso < 0 || (*skin)[i].size() > (*skin)[torso].size())) {
                torso = i;
            }
        }
        if (torso >= 0) {
            is_body[torso] = false;
        }

        auto rig = std::make_unique<RagdollRig>();
        rig->num_bones = num_bones;
        for (int k = 0; k < num_bones; ++k) {
            const int bone = ch.bone_order_list[k];
            if (!is_body[bone]) {
                rig->body_of[bone] = rig->body_of[parent[bone]];
                continue;
            }
            if (static_cast<int>(rig->bodies.size()) == ragdoll_max_bodies) {
                return nullptr;
            }
            rig->body_of[bone] = static_cast<int>(rig->bodies.size());
            RagdollRigBody& body = rig->bodies.emplace_back();
            body.bone = bone;
            body.parent_bone = parent[bone];
            body.parent_body = parent[bone] >= 0 ? rig->body_of[parent[bone]] : -1;
        }
        if (rig->bodies.size() < 2) {
            return nullptr;
        }

        std::array<float, ragdoll_max_bodies> chain{};
        for (int b = 0; b < static_cast<int>(rig->bodies.size()); ++b) {
            RagdollRigBody& body = rig->bodies[b];
            std::vector<btVector3> verts;
            for (int i = 0; i < num_bones; ++i) {
                if (rig->body_of[i] == b) {
                    verts.insert(verts.end(), (*skin)[i].begin(), (*skin)[i].end());
                }
            }
            const btVector3 tip = child_count[body.bone] > 0
                ? child_sum[body.bone] / static_cast<float>(child_count[body.bone])
                : bind[body.bone].getOrigin();
            fit_capsule(body, bind[body.bone], tip, verts);
            if (body.parent_bone >= 0 && !fit_joint(body, joint_samples(ch, body.bone))) {
                body.joint_in_parent_bone = (bind[body.parent_bone].inverse() * bind[body.bone]).getRotation();
            }
            // Along the chain: a raised arm reaches further from the root than it does in the bind pose.
            if (body.parent_body >= 0) {
                const int parent_head = rig->bodies[body.parent_body].bone;
                const btVector3 link = bind[body.bone].getOrigin() - bind[parent_head].getOrigin();
                chain[b] = chain[body.parent_body] + link.length();
            }
            const btVector3 centre = (bind[body.bone] * body.body_in_bone).getOrigin();
            const float extent = body.shape->getHalfHeight() + body.shape->getRadius();
            rig->bound_radius =
                std::max(rig->bound_radius, chain[b] + (centre - bind[body.bone].getOrigin()).length() + extent);
        }
        xlog::debug("[ragdoll] '{}': {} bones, {} bodies", ch.name, num_bones, rig->bodies.size());
        return rig;
    }

    const RagdollRig* rig_for(const rf::Character& ch)
    {
        const auto it = g_ragdoll_rigs.find(&ch);
        if (it != g_ragdoll_rigs.end()) {
            return it->second.get();
        }
        return g_ragdoll_rigs.emplace(&ch, build_rig(ch)).first->second.get();
    }

    // ─── Lifecycle ───────────────────────────────────────────────────────────

    Ragdoll* ragdoll_from_ci(const rf::CharacterInstance* ci)
    {
        if (g_ragdoll_by_ci.empty()) {
            return nullptr;
        }
        const auto it = g_ragdoll_by_ci.find(ci);
        return it != g_ragdoll_by_ci.end() ? it->second : nullptr;
    }

    rf::CharacterInstance* character_instance(const rf::VMesh* vmesh)
    {
        return vmesh && vmesh->type == rf::MESH_TYPE_CHARACTER ? static_cast<rf::CharacterInstance*>(vmesh->instance)
                                                               : nullptr;
    }

    Ragdoll* ragdoll_from_vmesh(const rf::VMesh* vmesh)
    {
        return ragdoll_from_ci(character_instance(vmesh));
    }

    void free_parts(Ragdoll& r)
    {
        if (r.parts[0].body) {
            g_vphys.world->release_predictive_manifolds();
        }
        for (RagdollPart& part : r.parts) {
            if (part.joint) {
                g_vphys.world->removeConstraint(part.joint.get());
                part.joint.reset();
            }
        }
        for (RagdollPart& part : r.parts) {
            if (part.body) {
                g_vphys.world->removeRigidBody(part.body.get());
                part.body.reset();
                part.motion.reset();
            }
        }
    }

    void ragdoll_erase(Ragdoll& r)
    {
        free_parts(r);
        g_ragdoll_by_ci.erase(r.ci);
        const auto it = std::find_if(g_ragdolls.begin(), g_ragdolls.end(),
                                     [&r](const std::unique_ptr<Ragdoll>& held) { return held.get() == &r; });
        if (it != g_ragdolls.end()) {
            g_ragdolls.erase(it);
        }
    }

    Ragdoll* track_instance(const rf::VMesh* vmesh, int owner_handle)
    {
        rf::CharacterInstance* ci = character_instance(vmesh);
        if (!ci || !ci->base_character || g_ragdoll_by_ci.contains(ci)) {
            return nullptr;
        }
        const RagdollRig* rig = rig_for(*ci->base_character);
        if (!rig) {
            return nullptr;
        }
        vphys_world_ensure_level_mesh();
        if (!level_mesh_active()) {
            return nullptr;
        }
        auto ragdoll = std::make_unique<Ragdoll>();
        ragdoll->ci = ci;
        ragdoll->rig = rig;
        ragdoll->owner_handle = owner_handle;
        Ragdoll* r = ragdoll.get();
        g_ragdoll_by_ci[ci] = r;
        g_ragdolls.push_back(std::move(ragdoll));
        return r;
    }

    void track_entity(rf::Entity& ep)
    {
        // A flagged entity's mesh is a corpse's or about to be freed, and a corpse mesh of its own would not
        // carry the posed instance over.
        if (!rf::entity_is_dying(&ep) || (ep.obj_flags & rf::OF_DELAYED_DELETE)
            || (ep.entity_flags & (rf::EF_GIB_ON_DEATH | rf::EF_IN_WATER | rf::EF_CUSTOM_CORPSE))
            || ep.material != rf::MATERIAL_FLESH || ep.info->corpse_vmesh_filename.c_str()[0] != '\0'
            || (!rf::is_multi && &ep == rf::local_player_entity)) {
            return;
        }
        if (Ragdoll* r = track_instance(ep.vmesh, ep.handle)) {
            r->entity_handle = ep.handle;
            r->killer_id = static_cast<uint8_t>(ep.killer_netid);
        }
    }

    bool is_exempt(const Ragdoll& r)
    {
        rf::Corpse* corpse = rf::corpse_from_handle(r.owner_handle);
        return corpse && rf::corpse_is_cap_exempt(corpse);
    }

    // A ragdoll still on its dying entity holds no corpse to delete, and the limit catches it once it does.
    void enforce_limit()
    {
        const auto is_live = [](const std::unique_ptr<Ragdoll>& r) { return !r->evicted && !is_exempt(*r); };
        // A save holds this many corpses at most.
        const int limit = rf::is_multi ? g_alpine_game_config.ragdolls_max
                                       : std::min<int>(g_alpine_game_config.ragdolls_max, rf::sr::MAX_CORPSES);
        auto excess = std::count_if(g_ragdolls.begin(), g_ragdolls.end(), is_live) - limit;
        for (auto it = g_ragdolls.begin(); excess > 0 && it != g_ragdolls.end(); ++it) {
            Ragdoll& r = **it;
            if (!is_live(*it) || !r.owner_is_corpse) {
                continue;
            }
            if (rf::Object* corpse = rf::obj_from_handle(r.owner_handle)) {
                free_parts(r);
                if (r.phase == RagdollPhase::simulating) {
                    r.phase = RagdollPhase::frozen;
                }
                r.evicted = true;
                rf::obj_flag_dead(corpse);
                --excess;
            }
        }
    }

    const RagdollHit* latest_hit(int entity_handle, int64_t now_ms)
    {
        const RagdollHit* latest = nullptr;
        for (const RagdollHit& hit : g_ragdoll_hits) {
            if (hit.entity_handle == entity_handle && now_ms - hit.time_ms < ragdoll_hit_window_ms
                && (!latest || hit.time_ms > latest->time_ms)) {
                latest = &hit;
            }
        }
        return latest;
    }

    const RagdollBlast* nearby_blast(const rf::Vector3& pos, int64_t now_ms)
    {
        const RagdollBlast* nearest = nullptr;
        float nearest_share = 1.0f;
        for (const RagdollBlast& blast : g_ragdoll_blasts) {
            if (blast.radius <= 0.0f || now_ms - blast.time_ms >= ragdoll_hit_window_ms) {
                continue;
            }
            const float share = (blast.pos - pos).len() / (blast.radius * ragdoll_blast_reach);
            if (share < nearest_share) {
                nearest = &blast;
                nearest_share = share;
            }
        }
        return nearest;
    }

    bool is_instant_death(const Ragdoll& r, const rf::Vector3& pos)
    {
        const int64_t now_ms = timer::get_i64(1000);
        const RagdollHit* hit = latest_hit(r.entity_handle, now_ms);
        return nearby_blast(pos, now_ms) || (hit && hit->damage >= ragdoll_big_hit_damage);
    }

    void kick_from_blast(Ragdoll& r, const RagdollBlast& blast)
    {
        const btVector3 centre = to_bt(blast.pos);
        const float reach = blast.radius * ragdoll_blast_reach;
        for (size_t b = 0; b < r.rig->bodies.size(); ++b) {
            btRigidBody& body = *r.parts[b].body;
            btVector3 away = body.getCenterOfMassPosition() - centre;
            const float falloff = std::max(1.0f - away.length() / reach, 0.0f);
            away = away.safeNormalize() + btVector3(0.0f, ragdoll_blast_lift, 0.0f);
            body.setLinearVelocity(body.getLinearVelocity() + away * (ragdoll_blast_kick * falloff));
        }
    }

    void kick_from_hit(Ragdoll& r, const rf::Vector3& hit_point, const rf::Vector3& hit_dir, float damage)
    {
        const btVector3 dir = to_bt(hit_dir);
        const btVector3 point = to_bt(hit_point);
        const float kick = ragdoll_hit_kick
                         * std::max(ragdoll_min_hit_kick_scale,
                                    std::min(damage / ragdoll_hit_kick_damage, ragdoll_max_hit_kick_scale));
        btRigidBody* struck = nullptr;
        float struck_dist = 0.0f;
        for (size_t b = 0; b < r.rig->bodies.size(); ++b) {
            btRigidBody& body = *r.parts[b].body;
            body.setLinearVelocity(body.getLinearVelocity() + dir * (kick * ragdoll_hit_kick_share));
            const float dist = (body.getCenterOfMassPosition() - point).length2();
            if (!struck || dist < struck_dist) {
                struck = &body;
                struck_dist = dist;
            }
        }
        struck->applyImpulse(dir * (kick * struck->getMass()), point - struck->getCenterOfMassPosition());
    }

    void kick_from_killer(Ragdoll& r, const rf::Object& owner)
    {
        const rf::Player* killer = rf::multi_find_player_by_id(r.killer_id);
        const rf::Entity* killer_ep = killer ? rf::entity_from_handle(killer->entity_handle) : nullptr;
        if (!killer_ep) {
            return;
        }
        const btVector3 dir = (to_bt(owner.pos) - to_bt(killer_ep->pos)).safeNormalize();
        for (size_t b = 0; b < r.rig->bodies.size(); ++b) {
            btRigidBody& body = *r.parts[b].body;
            body.setLinearVelocity(body.getLinearVelocity() + dir * ragdoll_killer_kick);
        }
    }

    void add_joint(Ragdoll& r, size_t b, const std::array<btTransform, ragdoll_max_bones>& bone_world)
    {
        const RagdollRigBody& rb = r.rig->bodies[b];
        btRigidBody& parent = *r.parts[rb.parent_body].body;
        btRigidBody& child = *r.parts[b].body;
        const btVector3 pivot = bone_world[rb.bone].getOrigin();
        const btTransform joint_a{bone_world[rb.parent_bone].getRotation() * rb.joint_in_parent_bone, pivot};
        const btTransform joint_b{bone_world[rb.bone].getRotation() * rb.joint_in_bone, pivot};
        auto joint = std::make_unique<btGeneric6DofConstraint>(
            parent, child, parent.getCenterOfMassTransform().inverse() * joint_a,
            child.getCenterOfMassTransform().inverse() * joint_b, true);
        joint->setLinearLowerLimit(btVector3(0.0f, 0.0f, 0.0f));
        joint->setLinearUpperLimit(btVector3(0.0f, 0.0f, 0.0f));
        joint->calculateTransforms();
        // A limit the pose at the switch starts past would snap the body.
        const float pad = ragdoll_limit_pad_deg * SIMD_PI / 180.0f;
        btVector3 low;
        btVector3 high;
        for (int axis = 0; axis < 3; ++axis) {
            const float cap = axis == 1 ? ragdoll_max_half_range_y : ragdoll_max_half_range;
            const float half = std::clamp(rb.half_range[axis] + pad, ragdoll_min_half_range, cap);
            const float angle = std::clamp(joint->getAngle(axis), -cap, cap);
            low[axis] = std::min(-half, angle);
            high[axis] = std::max(half, angle);
        }
        joint->setAngularLowerLimit(low);
        joint->setAngularUpperLimit(high);
        g_vphys.world->addConstraint(joint.get());
        r.parts[b].joint = std::move(joint);
    }

    void create_parts(Ragdoll& r, const std::array<btTransform, ragdoll_max_bones>& bone_world)
    {
        const RagdollRig& rig = *r.rig;
        for (size_t b = 0; b < rig.bodies.size(); ++b) {
            const RagdollRigBody& rb = rig.bodies[b];
            r.head_world[b] = bone_world[rb.bone];
            RagdollPart& part = r.parts[b];
            part.motion = std::make_unique<btDefaultMotionState>(bone_world[rb.bone] * rb.body_in_bone);

            btRigidBody::btRigidBodyConstructionInfo info{rb.mass, part.motion.get(), rb.shape.get(), rb.inertia};
            info.m_friction = ragdoll_friction;
            info.m_restitution = ragdoll_restitution;
            info.m_linearDamping = ragdoll_linear_damping;
            info.m_angularDamping = ragdoll_angular_damping;
            part.body = std::make_unique<btRigidBody>(info);
            part.body->setActivationState(DISABLE_DEACTIVATION);
            part.body->setCcdMotionThreshold(rb.shape->getRadius());
            part.body->setCcdSweptSphereRadius(rb.shape->getRadius() * 0.5f);
            g_vphys.world->addRigidBody(part.body.get(), vphys_group_ragdoll, vphys_group_level);
            // PER-BODY gravity, after addRigidBody stamps the world's.
            part.body->setGravity(btVector3(0.0f, -rf::gravity, 0.0f));
        }
        for (size_t b = 0; b < rig.bodies.size(); ++b) {
            if (rig.bodies[b].parent_body >= 0) {
                add_joint(r, b, bone_world);
            }
        }
        r.phase = RagdollPhase::simulating;
        r.still_time = 0.0f;
        r.sim_time = 0.0f;
    }

    void wake(Ragdoll& r)
    {
        if (r.phase != RagdollPhase::frozen || r.evicted) {
            return;
        }
        const RagdollRig& rig = *r.rig;
        std::array<btTransform, ragdoll_max_bones> bone_world;
        for (int i = 0; i < rig.num_bones; ++i) {
            bone_world[i] = r.head_world[rig.body_of[i]] * r.bone_in_head[i];
        }
        create_parts(r, bone_world);
    }

    void spawn_bodies(Ragdoll& r, const rf::Object& owner)
    {
        const RagdollRig& rig = *r.rig;
        const btTransform world = object_transform(owner);
        std::array<btTransform, ragdoll_max_bones> bone_world;
        for (int i = 0; i < rig.num_bones; ++i) {
            bone_world[i] = world * to_transform(r.pose[i]);
            const btTransform head = to_transform(r.pose[rig.bodies[rig.body_of[i]].bone]);
            r.bone_in_head[i] = head.inverse() * to_transform(r.pose[i]);
        }

        create_parts(r, bone_world);

        const btVector3 entity_vel = clamped(to_bt(owner.p_data.vel), ragdoll_max_entity_speed);
        for (size_t b = 0; b < rig.bodies.size(); ++b) {
            const RagdollRigBody& rb = rig.bodies[b];
            btVector3 linear = entity_vel;
            btVector3 angular{0.0f, 0.0f, 0.0f};
            if (r.sample_dt >= ragdoll_min_sample_dt) {
                btVector3 anim_linear;
                const btTransform from = world * to_transform(r.prev_pose[rb.bone]) * rb.body_in_bone;
                const btTransform to = bone_world[rb.bone] * rb.body_in_bone;
                btTransformUtil::calculateVelocity(from, to, r.sample_dt, anim_linear, angular);
                linear += clamped(anim_linear, ragdoll_max_anim_speed);
                angular = clamped(angular, ragdoll_max_anim_spin);
            }
            r.parts[b].body->setLinearVelocity(linear);
            r.parts[b].body->setAngularVelocity(angular);
        }

        const int64_t now_ms = timer::get_i64(1000);
        if (const RagdollBlast* blast = nearby_blast(owner.pos, now_ms)) {
            kick_from_blast(r, *blast);
        }
        else if (const RagdollHit* hit = latest_hit(r.entity_handle, now_ms)) {
            kick_from_hit(r, hit->point, hit->dir, hit->damage);
        }
        else if (rf::is_multi) {
            kick_from_killer(r, owner);
        }
    }

    float segment_distance_sq(const btVector3& p, const btVector3& d, const btVector3& q, const btVector3& e, float& s)
    {
        const btVector3 offset = p - q;
        const float dd = d.length2();
        const float ee = e.length2();
        const float de = d.dot(e);
        const float dr = d.dot(offset);
        const float er = e.dot(offset);
        const float denominator = dd * ee - de * de;
        if (denominator > SIMD_EPSILON) {
            s = std::clamp((de * er - dr * ee) / denominator, 0.0f, 1.0f);
        }
        else {
            s = dd > SIMD_EPSILON ? std::clamp(-dr / dd, 0.0f, 1.0f) : 0.0f;
        }
        float t = ee > SIMD_EPSILON ? (de * s + er) / ee : 0.0f;
        if (t < 0.0f || t > 1.0f) {
            t = t < 0.0f ? 0.0f : 1.0f;
            s = dd > SIMD_EPSILON ? std::clamp((de * t - dr) / dd, 0.0f, 1.0f) : 0.0f;
        }
        return (offset + d * s - e * t).length2();
    }

    // The bodies of a frozen ragdoll are gone, so this reads the pose rather than Bullet.
    bool path_strikes(const Ragdoll& r, const btVector3& from, const btVector3& path, float& at)
    {
        const RagdollRig& rig = *r.rig;
        const float bound = rig.bound_radius;
        float t = 0.0f;
        const btVector3 no_axis{0.0f, 0.0f, 0.0f};
        if (segment_distance_sq(from, path, r.head_world[0].getOrigin(), no_axis, t) > bound * bound) {
            return false;
        }
        bool struck = false;
        for (size_t b = 0; b < rig.bodies.size(); ++b) {
            const RagdollRigBody& rb = rig.bodies[b];
            const btTransform body = r.head_world[b] * rb.body_in_bone;
            const btVector3 half = body.getBasis().getColumn(1) * rb.shape->getHalfHeight();
            const float radius = rb.shape->getRadius();
            if (segment_distance_sq(from, path, body.getOrigin() - half, half * 2.0f, t) <= radius * radius
                && (!struck || t < at)) {
                struck = true;
                at = t;
            }
        }
        return struck;
    }

    void strike(Ragdoll& r, const rf::Weapon& wp, const rf::Vector3& point, const rf::Vector3& dir)
    {
        r.last_weapon_handle = wp.handle;
        wake(r);
        kick_from_hit(r, point, dir, weapon_damage(wp));
    }

    void react_to_shot(Ragdoll& r, const rf::Weapon& wp, const btVector3& from, const btVector3& path)
    {
        float at = 0.0f;
        if (!path_strikes(r, from, path, at)) {
            return;
        }
        const rf::Vector3 point = from_bt(from + path * at);
        const rf::Vector3 dir = from_bt(path.normalized());
        strike(r, wp, point, dir);
        rf::Object* owner = rf::obj_from_handle(r.owner_handle);
        // A body stays solid in single player, where stock bleeds the shots it stops.
        if (rf::is_multi && owner && r.owner_is_corpse) {
            const float blood = std::max(0.0f, std::min(weapon_damage(wp), ragdoll_max_blood_damage));
            rf::blood_splat_decal(blood, &point, &dir);
            rf::blood_spray(&point, owner->room, &owner->pos, blood);
        }
    }

    // Dead rounds count: one that hit something last frame still has that move to walk. Only a listen
    // host's lag-comp ghost has a path at full lifetime: a pierce moved it, the sim never did.
    void react_to_weapons()
    {
        for (rf::Weapon& wp : DoublyLinkedList{rf::weapon_list}) {
            if ((wp.info->flags & rf::WTF_MELEE) || wp.info->damage_radius >= ragdoll_min_blast_radius
                || wp.lifeleft_seconds >= wp.info->lifetime_seconds || !vehicle_vector_is_finite(wp.pos)
                || !vehicle_vector_is_finite(wp.last_pos)) {
                continue;
            }
            const btVector3 from = to_bt(wp.last_pos);
            const btVector3 path = to_bt(wp.pos) - from;
            if (path.length2() < ragdoll_min_weapon_path * ragdoll_min_weapon_path) {
                continue;
            }
            for (const std::unique_ptr<Ragdoll>& r : g_ragdolls) {
                if (r->phase != RagdollPhase::sampling && !r->evicted && r->last_weapon_handle != wp.handle) {
                    react_to_shot(*r, wp, from, path);
                }
            }
        }
    }

    bool level_blocks(const btVector3& from, const btVector3& to)
    {
        btCollisionWorld::ClosestRayResultCallback ray{from, to};
        ray.m_collisionFilterGroup = vphys_group_ragdoll;
        ray.m_collisionFilterMask = vphys_group_level;
        g_vphys.world->rayTest(from, to, ray);
        return ray.hasHit();
    }

    void push_from_blast(const RagdollBlast& blast, const btVector3& from)
    {
        const float reach = blast.radius * ragdoll_blast_reach;
        for (const std::unique_ptr<Ragdoll>& r : g_ragdolls) {
            if (r->phase == RagdollPhase::sampling || r->evicted) {
                continue;
            }
            const btVector3 root = r->head_world[0].getOrigin();
            if ((root - to_bt(blast.pos)).length2() < reach * reach && !level_blocks(from, root)) {
                wake(*r);
                kick_from_blast(*r, blast);
            }
        }
    }

    bool pose_is_rigid(const std::array<rf::Matrix43, ragdoll_max_bones>& pose, int num_bones)
    {
        return std::all_of(pose.begin(), pose.begin() + num_bones, [](const rf::Matrix43& bone) {
            return is_rigid(to_transform(bone), ragdoll_max_model_extent);
        });
    }

    void write_bones(const Ragdoll& r, rf::CharacterInstance* ci)
    {
        const rf::Object* owner = rf::obj_from_handle(r.owner_handle);
        if (!owner) {
            return;
        }
        const RagdollRig& rig = *r.rig;
        const btTransform to_model = object_transform(*owner).inverse();
        std::array<btTransform, ragdoll_max_bodies> head_model;
        for (size_t b = 0; b < rig.bodies.size(); ++b) {
            head_model[b] = to_model * r.head_world[b];
        }
        for (int i = 0; i < rig.num_bones; ++i) {
            const btTransform model = head_model[rig.body_of[i]] * r.bone_in_head[i];
            ci->bone_transforms_combined[i] = rf::Matrix43{from_bt(model.getBasis()), from_bt(model.getOrigin())};
            ci->bone_info[i].combined_update_iteration = ci->iteration_counter;
            // The stock pass rebuilds a final matrix only when this differs: a held pose never bumps the counter.
            ci->bone_info[i].final_update_iteration = static_cast<short>(ci->iteration_counter - 1);
        }
    }

    void update_owner_position(Ragdoll& r)
    {
        if (!r.owner_is_corpse) {
            return;
        }
        rf::Object* corpse = rf::obj_from_handle(r.owner_handle);
        rf::Vector3 pos = from_bt(r.head_world[0].getOrigin());
        if (corpse && (pos - r.room_pos).len() >= ragdoll_room_update_dist) {
            corpse->move(&pos);
            corpse->update_room();
            r.room_pos = pos;
        }
    }

    void bind_corpse(Ragdoll& r, rf::Corpse& corpse)
    {
        r.owner_handle = corpse.handle;
        r.owner_is_corpse = true;
        r.room_pos = corpse.pos;
        corpse.p_data.flags &= ~rf::PF_GRAVITY;
        corpse.p_data.vel = rf::Vector3{};
        if (r.phase != RagdollPhase::sampling) {
            update_owner_position(r);
            rf::ci_update_bone_transforms(r.ci);
        }
    }

    // A stock pose hangs below the corpse's position, which may be a ragdoll's pelvis, just off the floor.
    void lift_above_floor(rf::Corpse& corpse)
    {
        rf::CharacterInstance* ci = character_instance(corpse.vmesh);
        if (!ci || !ci->base_character) {
            return;
        }
        rf::ci_update_bone_transforms(ci);
        const btTransform world = object_transform(corpse);
        float lowest = 0.0f;
        for (int i = 0; i < std::min(ci->base_character->num_bones, ragdoll_max_bones); ++i) {
            const btTransform bone = world * to_transform(ci->bone_transforms_combined[i]);
            if (!is_rigid(bone, ragdoll_max_world_extent)) {
                return;
            }
            lowest = std::min(lowest, bone.getOrigin().y() - corpse.pos.y);
        }
        lowest -= ragdoll_floor_clearance;
        rf::Vector3 below = corpse.pos;
        below.y += lowest;
        rf::PCollisionOut hit{};
        if (vphys_collide_solid_segment(corpse.pos, below, rf::CF_PROCESS_INVISIBLE_FACES, hit)) {
            rf::Vector3 pos = corpse.pos;
            pos.y = hit.hit_point.y - lowest;
            corpse.move(&pos);
            corpse.update_room();
        }
    }

    void sample_pose(Ragdoll& r, float dt)
    {
        const rf::Object* owner = rf::obj_from_handle(r.owner_handle);
        if (!owner) {
            return;
        }
        rf::ci_update_bone_transforms(r.ci);
        r.prev_pose = r.pose;
        std::copy_n(r.ci->bone_transforms_combined, r.rig->num_bones, r.pose.begin());
        r.sample_dt = dt;
        r.age += dt;
        ++r.samples;
        if (r.samples >= 2 && (r.age * 1000.0f >= ragdoll_delay_ms || is_instant_death(r, owner->pos))
            && is_rigid(object_transform(*owner), ragdoll_max_world_extent)
            && pose_is_rigid(r.prev_pose, r.rig->num_bones) && pose_is_rigid(r.pose, r.rig->num_bones)) {
            // No entity means stock owned the corpse until now, and its pose may reach below the floor.
            rf::Corpse* corpse = r.entity_handle == -1 ? rf::corpse_from_handle(r.owner_handle) : nullptr;
            if (corpse) {
                lift_above_floor(*corpse);
            }
            spawn_bodies(r, *owner);
            if (corpse) {
                bind_corpse(r, *corpse);
            }
        }
    }

    // Stock leaves every corpse its gravity, so one without is a ragdoll's. A save keeps that but not the
    // pose, which comes back as the stock one.
    void restore_corpses()
    {
        for (rf::Corpse& corpse : DoublyLinkedList{rf::corpse_list}) {
            if ((corpse.p_data.flags & rf::PF_GRAVITY) || (corpse.obj_flags & rf::OF_DELAYED_DELETE)
                || ragdoll_from_vmesh(corpse.vmesh)) {
                continue;
            }
            if (Ragdoll* r = ragdolls_wanted() ? track_instance(corpse.vmesh, corpse.handle) : nullptr) {
                // No death to play out.
                r->age = ragdoll_delay_ms / 1000.0f;
            }
            else {
                lift_above_floor(corpse);
                corpse.p_data.flags |= rf::PF_GRAVITY;
                rf::obj_physics_activate(&corpse);
            }
        }
    }

    void update_from_bodies(Ragdoll& r, float dt)
    {
        const RagdollRig& rig = *r.rig;
        bool still = true;
        for (size_t b = 0; b < rig.bodies.size(); ++b) {
            const RagdollPart& part = r.parts[b];
            btTransform body_world;
            part.motion->getWorldTransform(body_world);
            if (!is_rigid(body_world, ragdoll_max_world_extent)) {
                r.sim_time = ragdoll_max_seconds;
                break;
            }
            r.head_world[b] = body_world * rig.bodies[b].bone_in_body;
            still = still && part.body->getLinearVelocity().length2() < ragdoll_freeze_speed * ragdoll_freeze_speed
                 && part.body->getAngularVelocity().length2() < ragdoll_freeze_spin * ragdoll_freeze_spin;
        }
        update_owner_position(r);
        // The shadow pass reads these ahead of the mesh render, which a culled mesh never reaches.
        rf::ci_update_bone_transforms(r.ci);
        // A crater still to land in the level mesh is about to take the floor away.
        r.still_time = still && !level_mesh_remesh_pending() ? r.still_time + dt : 0.0f;
        r.sim_time += dt;
        if (r.still_time >= ragdoll_freeze_seconds || r.sim_time >= ragdoll_max_seconds) {
            free_parts(r);
            r.phase = RagdollPhase::frozen;
        }
    }

    // ─── Hooks ───────────────────────────────────────────────────────────────

    FunHook<void(int, const rf::ubyte*, rf::CharacterInstance*)> ci_update_combined_bones_hook{
        0x0051B500,
        [](int num_bones, const rf::ubyte* bones, rf::CharacterInstance* ci) {
            const Ragdoll* r = ragdoll_from_ci(ci);
            if (r && r->phase != RagdollPhase::sampling) {
                write_bones(*r, ci);
                return;
            }
            ci_update_combined_bones_hook.call_target(num_bones, bones, ci);
        },
    };

    // The instance destructor's only caller: covers every way a mesh goes away.
    CallHook<void __fastcall(rf::CharacterInstance*)> ci_destroy_hook{
        0x00504603,
        [](rf::CharacterInstance* ci) FASTCALL_LAMBDA {
            if (Ragdoll* r = ragdoll_from_ci(ci)) {
                ragdoll_erase(*r);
            }
            ci_destroy_hook.call_target(ci);
        },
    };

    CallHook<rf::Corpse*(rf::Entity*, const char*, const rf::Vector3*, const rf::Matrix3*, int, int)>
        entity_die_corpse_create_ragdoll_hook{
            0x00419335,
            [](rf::Entity* ep, const char* pose_name, const rf::Vector3* pos, const rf::Matrix3* orient,
               int unk1, int unk2) -> rf::Corpse* {
                Ragdoll* r = ragdoll_from_vmesh(ep->vmesh);
                rf::Corpse* corpse =
                    entity_die_corpse_create_ragdoll_hook.call_target(ep, pose_name, pos, orient, unk1, unk2);
                if (r && corpse) {
                    bind_corpse(*r, *corpse);
                }
                return corpse;
            },
        };

    // The two counting walks of the stock corpse cap; the limit here replaces it for these.
    CallHook<bool(rf::Corpse*)> corpse_cap_exempt_hook{
        {0x00416F40u, 0x00416FAAu},
        [](rf::Corpse* corpse) {
            return corpse_cap_exempt_hook.call_target(corpse) || ragdoll_from_vmesh(corpse->vmesh) != nullptr;
        },
    };

    // A corpse can win this search, and Use does nothing on one in multiplayer, so a body swallows the press.
    CodeInjection player_find_use_target_skip_ragdoll_injection{
        0x00489B1A,
        [](auto& regs) {
            const rf::Corpse* corpse = regs.esi;
            if (ragdoll_is_ghost(corpse)) {
                regs.eip = 0x00489BC7;
            }
        },
    };

    // The corpses inside a door's trigger hold it open and fade out; a ragdoll exists on this machine alone.
    const auto mover_obstruction_skip_ragdoll = [](auto& regs) {
        const rf::Corpse* corpse = regs.edi;
        if (ragdoll_is_ghost(corpse)) {
            regs.eip = 0x0046A3B7;
        }
    };
    CodeInjection mover_obstruction_box_skip_ragdoll_injection{0x0046A368, mover_obstruction_skip_ragdoll};
    CodeInjection mover_obstruction_radius_skip_ragdoll_injection{0x0046A395, mover_obstruction_skip_ragdoll};

    CallHook<bool(rf::AiInfo*, int)> ai_corpse_pickup_ragdoll_hook{
        0x004A172D,
        [](rf::AiInfo* ai, int corpse_handle) {
            const bool carried = ai_corpse_pickup_ragdoll_hook.call_target(ai, corpse_handle);
            if (rf::Corpse* corpse = carried ? rf::corpse_from_handle(corpse_handle) : nullptr) {
                if (Ragdoll* r = ragdoll_from_vmesh(corpse->vmesh)) {
                    ragdoll_erase(*r);
                }
                corpse->p_data.flags |= rf::PF_GRAVITY;
            }
            return carried;
        },
    };

    CallHook<bool(rf::AiInfo*, bool)> ai_corpse_drop_ragdoll_hook{
        0x004A167C,
        [](rf::AiInfo* ai, bool force) {
            const int corpse_handle = ai->corpse_carry_handle;
            const bool dropped = ai_corpse_drop_ragdoll_hook.call_target(ai, force);
            if (rf::Corpse* corpse = dropped && ragdolls_wanted() ? rf::corpse_from_handle(corpse_handle) : nullptr) {
                if (Ragdoll* r = track_instance(corpse->vmesh, corpse->handle)) {
                    // The first sample comes ahead of the drop anim's first frame.
                    r->samples = -1;
                }
            }
            return dropped;
        },
    };

    // This plays on every machine, unlike the server's radius damage.
    FunHook<void(int, int, void*, rf::Vector3*, rf::Vector3*, int)> weapon_create_impact_vclip_hook{
        0x004C8A10,
        [](int weapon_type, int room, void* src_pos, rf::Vector3* hit_point, rf::Vector3* hit_normal, int parent) {
            weapon_create_impact_vclip_hook.call_target(weapon_type, room, src_pos, hit_point, hit_normal, parent);
            if (ragdolls_wanted() && hit_point && vehicle_vector_is_finite(*hit_point) && weapon_type >= 0
                && weapon_type < rf::num_weapon_types
                && rf::weapon_types[weapon_type].damage_radius >= ragdoll_min_blast_radius) {
                const RagdollBlast blast{*hit_point, rf::weapon_types[weapon_type].damage_radius, timer::get_i64(1000)};
                g_ragdoll_blasts[g_ragdoll_next_blast++ % g_ragdoll_blasts.size()] = blast;
                // The point is on the surface, where a ray from it would stop at once.
                const bool has_normal = hit_normal && vehicle_vector_is_finite(*hit_normal);
                const btVector3 lift = has_normal ? to_bt(*hit_normal) * ragdoll_blast_surface_lift
                                                  : btVector3{0.0f, ragdoll_blast_surface_lift, 0.0f};
                push_from_blast(blast, to_bt(blast.pos) + lift);
            }
        },
    };

    ConsoleCommand2 cl_ragdolls_cmd{
        "cl_ragdolls",
        [](std::optional<bool> enabled) {
            setting_console_set_bool("ragdolls", enabled);
            rf::console::print("Ragdoll deaths are {}", g_alpine_game_config.ragdolls ? "enabled" : "disabled");
        },
        "Toggle client-side ragdoll deaths",
        "cl_ragdolls [0|1]",
    };

    ConsoleCommand2 cl_ragdolls_max_cmd{
        "cl_ragdolls_max",
        [](std::optional<int> value) {
            if (value) {
                setting_console_set("ragdolls_max", std::to_string(*value));
            }
            rf::console::print("Ragdoll limit is {}", g_alpine_game_config.ragdolls_max);
        },
        "Set how many ragdolls stay in the world before the oldest is removed; capped to 32 in single player",
        "cl_ragdolls_max <4-128>",
    };
} // namespace

bool ragdolls_wanted()
{
    return !rf::is_dedicated_server && g_alpine_game_config.ragdolls;
}

// A body stays solid in single player; in multiplayer the ragdoll exists on this machine alone.
bool ragdoll_is_ghost(const rf::Object* obj)
{
    return rf::is_multi && obj && obj->type == rf::OT_CORPSE && ragdoll_from_vmesh(obj->vmesh);
}

void ragdoll_note_weapon_hit(const rf::Weapon* wp)
{
    const int handle = wp->p_data.collide_out.obj_handle;
    if (!ragdolls_wanted() || handle == -1 || !vehicle_vector_is_finite(wp->p_data.collide_out.hit_point)
        || !vehicle_vector_is_finite(wp->orient.fvec)) {
        return;
    }
    const RagdollHit hit{
        handle, wp->p_data.collide_out.hit_point, wp->orient.fvec, weapon_damage(*wp), timer::get_i64(1000)};
    g_ragdoll_hits[g_ragdoll_next_hit++ % g_ragdoll_hits.size()] = hit;
    // A solid body stops the round at its collision spheres, which can be short of the capsules.
    const rf::Object* obj = rf::obj_from_handle(handle);
    Ragdoll* r = obj && obj->type == rf::OT_CORPSE ? ragdoll_from_vmesh(obj->vmesh) : nullptr;
    if (r && r->phase != RagdollPhase::sampling && !r->evicted && r->last_weapon_handle != wp->handle) {
        strike(*r, *wp, hit.point, hit.dir);
    }
}

void ragdoll_after_entity_dead(rf::Entity* ep)
{
    // Stock may leave no corpse, which would delete the mesh. A body is gameplay in single player, so
    // there the ragdoll goes with it.
    Ragdoll* r = ragdoll_from_vmesh(ep->vmesh);
    if (!r || r->owner_is_corpse || (ep->entity_flags & rf::EF_GIB_ON_DEATH)) {
        return;
    }
    if (rf::Corpse* corpse = rf::is_multi ? rf::corpse_create(ep, "", &ep->pos, &ep->orient, 0, 0) : nullptr) {
        bind_corpse(*r, *corpse);
    }
    else {
        // corpse_create claims the mesh before it can fail.
        ep->obj_flags = static_cast<rf::ObjectFlags>(ep->obj_flags & ~rf::OF_MESH_NOT_OWNED);
        ragdoll_erase(*r);
    }
}

void ragdoll_pre_step(float dt)
{
    if (dt <= 0.0f) {
        return;
    }
    if (!rf::is_multi) {
        restore_corpses();
    }
    if (ragdolls_wanted()) {
        for (rf::Entity& ep : DoublyLinkedList{rf::entity_list}) {
            track_entity(ep);
        }
    }
    if (g_ragdolls.empty()) {
        return;
    }
    enforce_limit();
    for (const std::unique_ptr<Ragdoll>& r : g_ragdolls) {
        if (r->phase == RagdollPhase::sampling && !r->evicted) {
            sample_pose(*r, dt);
        }
    }
    if (ragdolls_wanted()) {
        react_to_weapons();
    }
}

bool ragdoll_needs_step()
{
    return std::any_of(g_ragdolls.begin(), g_ragdolls.end(), [](const std::unique_ptr<Ragdoll>& r) {
        return r->phase == RagdollPhase::simulating;
    });
}

void ragdoll_post_step(float dt)
{
    for (const std::unique_ptr<Ragdoll>& r : g_ragdolls) {
        if (r->phase == RagdollPhase::simulating) {
            update_from_bodies(*r, dt);
        }
    }
}

void ragdoll_wake_near(const rf::Vector3& pos, float radius)
{
    const btVector3 centre = to_bt(pos);
    const btVector3 extent{radius, radius, radius};
    ragdoll_wake_in_box(centre - extent, centre + extent);
}

void ragdoll_wake_in_box(const btVector3& lo, const btVector3& hi)
{
    for (const std::unique_ptr<Ragdoll>& r : g_ragdolls) {
        if (r->phase != RagdollPhase::frozen) {
            continue;
        }
        const btVector3 root = r->head_world[0].getOrigin();
        btVector3 nearest = root;
        nearest.setMax(lo);
        nearest.setMin(hi);
        const float reach = r->rig->bound_radius;
        if ((nearest - root).length2() < reach * reach) {
            wake(*r);
        }
    }
}

void ragdoll_destroy_all()
{
    while (!g_ragdolls.empty()) {
        ragdoll_erase(*g_ragdolls.back());
    }
    g_ragdoll_rigs.clear();
}

void ragdoll_apply_patches()
{
    ci_update_combined_bones_hook.install();
    ci_destroy_hook.install();
    entity_die_corpse_create_ragdoll_hook.install();
    corpse_cap_exempt_hook.install();
    player_find_use_target_skip_ragdoll_injection.install();
    mover_obstruction_box_skip_ragdoll_injection.install();
    mover_obstruction_radius_skip_ragdoll_injection.install();
    ai_corpse_pickup_ragdoll_hook.install();
    ai_corpse_drop_ragdoll_hook.install();
    weapon_create_impact_vclip_hook.install();
    cl_ragdolls_cmd.register_cmd();
    cl_ragdolls_max_cmd.register_cmd();
}
