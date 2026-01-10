#pragma once

#include <patch_common/MemUtils.h>
#include "../os/vtypes.h"
#include "../os/string.h"

namespace rf
{
    constexpr std::size_t max_path_len = 256;

    class File
    {
        using SelfType = File;

        char m_internal_data[0x114];

    public:
        enum SeekOrigin {
            seek_set = 0,
            seek_cur = 1,
            seek_end = 2,
        };

        static constexpr uint mode_read = 1;
        static constexpr uint mode_write = 2;
        static constexpr uint mode_text = 0x80000000;

        static constexpr uint default_path_id = 9999999;


        File()
        {
            AddrCaller{0x00523940}.this_call(this);
        }

        ~File()
        {
            AddrCaller{0x00523960}.this_call(this);
        }

        int open(const char* filename, int mode = mode_read, int path_id = default_path_id)
        {
            return AddrCaller{0x00524190}.this_call<int>(this, filename, mode, path_id);
        }

        bool find(const char* filename, int path_id = default_path_id)
        {
            return AddrCaller{0x00523CE0}.this_call<bool>(this, filename, path_id);
        }

        void close()
        {
            AddrCaller{0x005242A0}.this_call(this);
        }

        [[nodiscard]] int get_version() const
        {
            return AddrCaller{0x005239C0}.this_call<int>(this);
        }

        [[nodiscard]] bool check_version(int min_ver) const
        {
            return AddrCaller{0x00523990}.this_call<bool>(this, min_ver);
        }

        [[nodiscard]] int error() const
        {
            return AddrCaller{0x00524530}.this_call<bool>(this);
        }

        int seek(int pos, SeekOrigin origin)
        {
            return AddrCaller{0x00524400}.this_call<int>(this, pos, origin);
        }

        [[nodiscard]] int tell() const
        {
            return AddrCaller{0x005244E0}.this_call<int>(this);
        }

        [[nodiscard]] int size(const char *filename = nullptr, int a3 = default_path_id) const
        {
            return AddrCaller{0x00524370}.this_call<int>(this, filename, a3);
        }

        int read(void *buf, int buf_len, int min_ver = 0, int unused = 0)
        {
            return AddrCaller{0x0052CF60}.this_call<int>(this, buf, buf_len, min_ver, unused);
        }

        void read_vector(Vector3* mat, int ver, Vector3* deflt)
        {
            AddrCaller{0x0052CA00}.this_call(this, mat, ver, deflt);
        }

        void read_matrix(Matrix3* mat, int ver, Matrix3* deflt)
        {
            AddrCaller{0x0052CAC0}.this_call(this, mat, ver, deflt);
        }

        void read_string(String* mat, int ver, String* deflt)
        {
            AddrCaller{0x0052CC10}.this_call(this, mat, ver, deflt);
        }

        float read_float(int min_ver, float def_val)
        {
            return AddrCaller{0x0052C9B0}.this_call<float>(this, min_ver, def_val);
        }

        int read_int(int min_ver, int def_val)
        {
            return AddrCaller{0x0052C910}.this_call<int>(this, min_ver, def_val);
        }

        bool read_bool(int min_ver, bool def_val)
        {
            return AddrCaller{0x0052C780}.this_call<bool>(this, min_ver, def_val);
        }

        template<typename T>
        T read(int min_ver = 0, T def_val = 0)
        {
            if (check_version(min_ver)) {
                T val;
                read(&val, sizeof(val));
                if (!error()) {
                    return val;
                }
            }
            return def_val;
        }
    };

    static auto& file_get_ext = addr_as_ref<char*(const char *path)>(0x005143F0);
    static auto& file_add_path = addr_as_ref<int(const char *path, const char *exts, bool search_on_cd)>(0x00514070);
    //static auto& game_add_path = addr_as_ref<int(const char* path, const char* exts)>(0x004B1330);

    static auto& root_path = addr_as_ref<char[max_path_len]>(0x018060E8);
}
