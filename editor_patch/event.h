#pragma once

#include <map>
#include <vector>
#include <string>

struct AlpineEventDialog
{
    std::string script_name_field;
    std::string delay_field;
    int int1_field;
    int int2_field;
    std::string float1_field;
    std::string float2_field;
    bool bool1_field;
    bool bool2_field;
    std::string str1_field;
    std::string str2_field;
};

// level editor alpine event IDs, separate from EventType in game
enum class AlpineDedEventID : int
{
    Set_Variable = 89,
    Clone_Entity,
    Set_Player_World_Collide,
    Switch_Random,
    Difficulty_Gate,
    HUD_Message,
    Play_Video,
    Set_Level_Hardness,
    Sequence,
    Clear_Queued,
    Remove_Link,
    Route_Node,
    Add_Link,
    Valid_Gate,
    Goal_Math,
    Goal_Gate,
    Scope_Gate,
    Inside_Gate,
    Anchor_Marker,
    Force_Unhide,
    Set_Difficulty,
    Set_Fog_Far_Clip,
    AF_When_Dead,
    Gametype_Gate,
    When_Picked_Up,
    Set_Skybox,
    Set_Life,
    Set_Debris,
    Set_Fog_Color,
    Set_Entity_Flag,
    AF_Teleport_Player,
    Set_Item_Drop,
    AF_Heal,
    Anchor_Marker_Orient,
    Light_State,
    World_HUD_Sprite,
    Set_Light_Color,
    Capture_Point_Handler,
    Respawn_Point_State,
    Modify_Respawn_Point,
    When_Captured,
    Set_Capture_Point_Owner,
    Owner_Gate,
    Set_Gameplay_Rule,
    When_Round_Ends
};

constexpr int af_ded_event_to_int(AlpineDedEventID event_id) noexcept
{
    return static_cast<int>(event_id);
}

constexpr AlpineDedEventID int_to_af_ded_event(int event_id) noexcept
{
    return static_cast<AlpineDedEventID>(event_id);
}

enum FieldType
{
    FIELD_SCRIPT_NAME = 1001,
    FIELD_DELAY = 1002,
    FIELD_INT1 = 1003,
    FIELD_INT2 = 1004,
    FIELD_FLOAT1 = 1005,
    FIELD_FLOAT2 = 1006,
    FIELD_BOOL1 = 1007,
    FIELD_BOOL2 = 1008,
    FIELD_STR1 = 1009,
    FIELD_STR2 = 1010
};

// Field configuration for each event_type
struct FieldConfig
{
    std::vector<FieldType> fieldsToShow;
    std::map<FieldType, std::string> fieldLabels;
    std::map<FieldType, std::vector<std::string>> dropdownItems;
    std::map<FieldType, bool> dropdownSaveIndex;
};

static auto OpenLinksDialog = reinterpret_cast<void(__thiscall*)(void* this_)>(0x004073D0);
