#include <cstddef>
#include <cstring>
#include <functional>
#include <string_view>
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <memory>
#include <iomanip>
#include <sstream>
#include <string>
#include <common/version/version.h>
#include <common/config/BuildConfig.h>
#include <common/utils/os-utils.h>
#include <xlog/xlog.h>
#include <xlog/ConsoleAppender.h>
#include <xlog/FileAppender.h>
#include <xlog/Win32Appender.h>
#include <patch_common/MemUtils.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>
#include <crash_handler_stub.h>
#include "../game_patch/rf/os/array.h"
#include "exports.h"
#include "resources.h"
#include "level.h"
#include "mfc_types.h"
#include "vtypes.h"
#include "event.h"

//extern "C" IMAGE_DOS_HEADER __ImageBase;
//#define HINST_THISCOMPONENT ((HINSTANCE) & __ImageBase)

// Custom event support
constexpr int original_event_count = 89;
constexpr int new_event_count = 45; // must be 1 higher than actual count
constexpr int total_event_count = original_event_count + new_event_count;
std::unique_ptr<const char*[]> extended_event_names; // array to hold original + additional event names

// master list of new events, last one is dummy for counting (ignore)
const char* additional_event_names[new_event_count] = {
    "Set_Variable",
    "Clone_Entity",
    "Set_Player_World_Collide",
    "Switch_Random",
    "Difficulty_Gate",
    "HUD_Message",
    "Play_Video",
    "Set_Level_Hardness",
    "Sequence",
    "Clear_Queued",
    "Remove_Link",
    "Route_Node",
    "Add_Link",
    "Valid_Gate",
    "Goal_Math",
    "Goal_Gate",
    "Scope_Gate",
    "Inside_Gate",
    "Anchor_Marker",
    "Force_Unhide",
    "Set_Difficulty",
    "Set_Fog_Far_Clip",
    "AF_When_Dead",
    "Gametype_Gate",
    "When_Picked_Up",
    "Set_Skybox",
    "Set_Life",
    "Set_Debris",
    "Set_Fog_Color",
    "Set_Entity_Flag",
    "AF_Teleport_Player",
    "Set_Item_Drop",
    "AF_Heal",
    "Anchor_Marker_Orient",
    "Light_State",
    "World_HUD_Sprite",
    "Set_Light_Color",
    "Capture_Point_Handler",
    "Respawn_Point_State",
    "Modify_Respawn_Point",
    "When_Captured",
    "Set_Capture_Point_Owner",
    "Owner_Gate",
    "Set_Gameplay_Rule",
    "_dummy"
};

void initialize_event_names()
{
    // allocate space for total event names
    extended_event_names = std::make_unique<const char*[]>(total_event_count + 1);
    extended_event_names[total_event_count] = nullptr; // padding to prevent overrun

    // reference the stock event names array in memory
    const char** original_event_names = reinterpret_cast<const char**>(0x00578B78);

    // read and populate extended_event_names with stock event names
    for (int i = 0; i < original_event_count; ++i) {
        if (original_event_names[i]) {
            extended_event_names[i] = original_event_names[i];
            //xlog::info("Loaded original event name [{}]: {}", i, original_event_names[i]);
        }
        else { // should never be hit, including for safety
            //xlog::warn("Original event name [{}] is null or corrupted", i);
            extended_event_names[i] = nullptr;
        }
    }

    // add new event names to extended_event_names
    for (int i = 0; i < new_event_count; ++i) {
        extended_event_names[original_event_count + i] = additional_event_names[i];
        //xlog::info("Added additional event name [{}]: {}", original_event_count + i, additional_event_names[i]);
    }

    xlog::info("Initialized extended_event_names with {} entries", total_event_count);
}

// verify the event names in extended_event_names LOGGING
void debug_event_names()
{
    for (int i = 0; i < total_event_count; ++i) {
        if (extended_event_names[i]) {
            //xlog::info("Debug: Event name [{}]: {}", i, extended_event_names[i]);
        }
        else {
            xlog::warn("Debug: Event name [{}] is null or corrupted", i);
        }
    }
};

// verify the event names in extended_event_names LOGGING
void verify_event_names()
{
    for (int i = 0; i < total_event_count; ++i) {
        if (extended_event_names[i]) {
            //xlog::info("Event name [{}]: {}", i, extended_event_names[i]);
        }
        else {
            xlog::warn("Event name [{}] is null or corrupted", i);
        }
    }
}

// in CDedLevel__OpenEventPropertiesInternal
CodeInjection event_names_injection{
    0x00407782,
    [](auto& regs) {
        using namespace asm_regs;

        // look up index for selected event in new array
        int index = regs.eax;
        regs.edx = reinterpret_cast<uintptr_t>(extended_event_names[index]);

        regs.eip = 0x00407789;
    }
};

// in CEventDialog__OnInitDialog
CodeInjection OnInitDialog_redirect_event_names{
    0x004617EA, [](auto& regs) {
        // update reference to old event_names array with new extended_event_names
        regs.edi = reinterpret_cast<uintptr_t>(extended_event_names.get());

        for (int i = 0; i < total_event_count; ++i) {
            //xlog::info("Attempting to access extended_event_names[{}]: {}", i, extended_event_names[i]);
        }

        regs.eip = 0x004617EF;
    }
};

// in get_event_type_from_class_name
CodeInjection get_event_type_redirect_event_names{
    0x004516A9,
    [](auto& regs) {
        using namespace asm_regs;
        // update reference to old event_names array with new extended_event_names
        regs.esi = reinterpret_cast<uintptr_t>(extended_event_names.get());

        for (int i = 0; i < total_event_count; ++i) {
            //xlog::info("Also Attempting to access extended_event_names[{}]: {}", i, extended_event_names[i]);
        }
      
        regs.eip = 0x004516AE;
    }
};

std::string generate_window_title(const DedEvent* event)
{
    return "Alpine Event Properties - " + std::string(event->class_name.c_str()) +
        " (" + std::to_string(event->uid) + ")";
}

bool is_valid_float(const std::string& str, float& outValue)
{
    try {
        size_t idx;
        outValue = std::stof(str, &idx);
        return idx == str.size();
    }
    catch (const std::exception&) {
        return false;
    }
}

// temporary storage of currently active alpine event and level
DedEvent* currentDedEvent = nullptr;
CDedLevel* currentDedLevel = nullptr; // used by links dialog

std::map<AlpineDedEventID, FieldConfig> eventFieldConfigs = {
    {AlpineDedEventID::Set_Variable, {
        {FIELD_INT1, FIELD_INT2, FIELD_FLOAT1, FIELD_BOOL1, FIELD_STR1},
        {
            {FIELD_INT1, "Variable handle:"},
            {FIELD_INT2, "Value for int1 or int2:"},
            {FIELD_FLOAT1, "Value for delay, float1, or float2:"},
            {FIELD_BOOL1, "Value for bool1 or bool2:"},
            {FIELD_STR1, "Value for str1 or str2:"}
        },
        {
            {FIELD_INT1,
                {
                    "delay",
                    "int1",
                    "int2",
                    "float1",
                    "float2",
                    "bool1",
                    "bool2",
                    "str1",
                    "str2"
                }
            }
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Clone_Entity, {
        {FIELD_BOOL1, FIELD_BOOL2, FIELD_INT1},
        {
            {FIELD_BOOL1, "Clone is hostile to player (bool1):"},
            {FIELD_BOOL2, "Go to player (bool2):"},
            {FIELD_INT1, "Link event UID to clone (int1):"}
        }
    }},
    {AlpineDedEventID::Switch_Random, {
        {FIELD_BOOL1},
        {
            {FIELD_BOOL1, "No repeats until all used (bool1):"}
        }
    }},
    {AlpineDedEventID::Difficulty_Gate, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Difficulty (int1):"}
        },
        {
            {FIELD_INT1,
            {"Easy", "Medium", "Hard", "Impossible"}}
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::HUD_Message, {
        {FIELD_STR1, FIELD_FLOAT1},
        {
            {FIELD_STR1, "Message text (str1):"},
            {FIELD_FLOAT1, "Duration (float1):"}
        }
    }},
    {AlpineDedEventID::Play_Video, {
        {FIELD_STR1},
        {
            {FIELD_STR1, "Video filename (str1):"}
        }
    }},
    {AlpineDedEventID::Set_Level_Hardness, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Hardness (int1):"}
        }
    }},
    {AlpineDedEventID::Sequence, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Next index to activate (int1):"}
        }
    }},
    {AlpineDedEventID::Goal_Gate, {
        {FIELD_STR1, FIELD_INT1, FIELD_INT2},
        {
            {FIELD_STR1, "Goal to test (str1):"},
            {FIELD_INT1, "Test to run (int1):"},
            {FIELD_INT2, "Value to test against (int2):"}
        },
        {
            {FIELD_INT1,
                {"Equal to",
                "Not equal to",
                "Greater than",
                "Less than",
                "Greater than or equal to",
                "Less than or equal to",
                "Is odd",
                "Is even",
                "Divisible by",
                "Less than initial value",
                "Greater than initial value",
                "Less or equal initial value",
                "Greater or equal initial value",
                "Equal to initial value"
                }
            }
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Remove_Link, {
        {FIELD_BOOL1},
        {
            {FIELD_BOOL1, "Purge all links (bool1):"}
        }
    }},
    {AlpineDedEventID::Route_Node, {
        {FIELD_INT1, FIELD_BOOL1, FIELD_BOOL2},
        {
            {FIELD_INT1, "Node behavior (int1):"},
            {FIELD_BOOL1, "Non-retriggerable delay (bool1):"},
            {FIELD_BOOL2, "Clear trigger info (bool2):"}
        },
        {
            {FIELD_INT1,
                {"Pass through",
                "Drop",
                "Invert",
                "Force on",
                "Force off"
                }
            }
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Add_Link, {
        {FIELD_INT1, FIELD_BOOL1},
        {
            {FIELD_INT1, "Source event UID (int1):"},
            {FIELD_BOOL1, "Link inbound (bool1):"}
        }
    }},
    {AlpineDedEventID::Valid_Gate, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Object UID to test (int1):"}
        }
    }},
    {AlpineDedEventID::Goal_Math, {
        {FIELD_STR1, FIELD_INT1, FIELD_INT2},
        {
            {FIELD_STR1, "Goal to edit (str1):"},
            {FIELD_INT1, "Operation to perform (int1):"},
            {FIELD_INT2, "Value to use for operation (int2):"}
        },
        {
            {FIELD_INT1,
                {"Add to goal",
                "Subtract from goal",
                "Multiply by goal",
                "Divide goal by",
                "Divide by goal",
                "Set goal to",
                "Modulo goal by",
                "Raise goal to power",
                "Negate goal",
                "Absolute value of goal",
                "Max of goal and value",
                "Min of goal and value",
                "Reset goal to initial value"
                }
            }
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Scope_Gate, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Scope to test against (int1):"}
        },
        {
            {FIELD_INT1,
                {"Multiplayer",
                "Single player",
                "Server",
                "Dedicated server",
                "Client",
                "Triggering player",
                "Red team",
                "Blue team",
                "Player that has flag"
                }
            }
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Inside_Gate, {
        {FIELD_INT1, FIELD_INT2},
        {
            {FIELD_INT1, "UID (trigger/room) to check (int1):"},
            {FIELD_INT2, "What to check for (int2):"}
        },
        {
            {FIELD_INT2,
                {"Player",
                "Entity that triggered this",
                "All linked objects",
                "At least 1 linked object"
                }
            }
        },
        {
            {FIELD_INT2, true}
        }
    }},
    {AlpineDedEventID::Set_Difficulty, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Difficulty (int1):"}
        },
        {
            {FIELD_INT1,
            {"Easy", "Medium", "Hard", "Impossible"}}
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Set_Fog_Far_Clip, {
        {FIELD_FLOAT1},
        {
            {FIELD_FLOAT1, "Far clip distance (float1):"}
        }
    }},
    {AlpineDedEventID::AF_When_Dead, {
        {FIELD_BOOL1},
        {
            {FIELD_BOOL1, "Activate on any dead (bool1):"}
        }
    }},
    {AlpineDedEventID::Gametype_Gate, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Check for gametype (int1):"}
        },
        {
            {FIELD_INT1,
            {"Deathmatch",
            "Capture the Flag",
            "Team Deathmatch",
            "King of the Hill",
            "Damage Control",
            "Revolt",
            "Run",
            "Escalation"}}
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Set_Skybox, {
        {FIELD_INT1, FIELD_INT2, FIELD_BOOL1, FIELD_FLOAT1},
        {
            {FIELD_INT1, "Skybox room UID (int1):"},
            {FIELD_INT2, "Eye anchor UID (int2):"},
            {FIELD_BOOL1, "Use relative position (bool1):"},
            {FIELD_FLOAT1, "Relative position scale (float1):"}
        }
    }},
    {AlpineDedEventID::Set_Life, {
        {FIELD_FLOAT1},
        {
            {FIELD_FLOAT1, "New life value (float1):"}
        }
    }},
    {AlpineDedEventID::Set_Debris, {
        {FIELD_STR1, FIELD_INT1, FIELD_FLOAT1, FIELD_STR2, FIELD_FLOAT2},
        {
            {FIELD_STR1, "Debris filename (str1):"},
            {FIELD_INT1, "Explosion VClip index (int1):"},
            {FIELD_FLOAT1, "Explosion VClip radius (float1):"},
            {FIELD_STR2, "Debris sound set (str2):"},
            {FIELD_FLOAT2, "Debris velocity (float2):"}
        }
    }},
    {AlpineDedEventID::Set_Fog_Color, {
        {FIELD_STR1},
        {
            {FIELD_STR1, "Fog color (str1):"}
        }
    }},
    {AlpineDedEventID::Set_Entity_Flag, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Flag to set (int1):"}
        },
        {
            {FIELD_INT1,
            {"Boarded (vehicles only)",
            "Cower from weapon",
            "Question unarmed player",
            "Fade corpse immediately",
            "Don't hum",
            "No shadow",
            "Perfect aim",
            "Permanent corpse",
            "Always face player",
            "Only attack player",
            "Deaf",
            "Ignore terrain when firing"}}
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::AF_Teleport_Player, {
        {FIELD_BOOL1, FIELD_BOOL2, FIELD_STR1, FIELD_STR2},
        {
            {FIELD_BOOL1, "Reset player velocity (bool1):"},
            {FIELD_BOOL2, "Eject player from vehicle (bool2):"},
            {FIELD_STR1, "Entrance VClip (str1):"},
            {FIELD_STR2, "Exit VClip (str2):"}
        }
    }},
    {AlpineDedEventID::Set_Item_Drop, {
        {FIELD_STR1},
        {
            {FIELD_STR1, "Item class to drop (str1):"}
        }
    }},
    {AlpineDedEventID::AF_Heal, {
        {FIELD_INT1, FIELD_INT2, FIELD_BOOL1, FIELD_BOOL2},
        {
            {FIELD_INT1, "Amount (int1):"},
            {FIELD_INT2, "Apply change to (int2):"},
            {FIELD_BOOL1, "Apply to armor instead (bool1):"},
            {FIELD_BOOL2, "Allow super values (bool2):"}
        },
        {
            {FIELD_INT2,
            {"Linked entities",
            "Triggering player",
            "All players",
            "Teammates",
            "Enemy team",
            "Players on red team",
            "Players on blue team"}}
        },
        {
            {FIELD_INT2, true}
        }
    }},
    {AlpineDedEventID::World_HUD_Sprite, {
        {FIELD_BOOL1, FIELD_INT1, FIELD_FLOAT1, FIELD_STR1, FIELD_STR2},
        {
            {FIELD_BOOL1, "Start enabled:"},
            {FIELD_INT1, "Render mode:"},
            {FIELD_FLOAT1, "Render scale:"},
            {FIELD_STR1, "Sprite filename:"},
            {FIELD_STR2, "Sprite filename blue:"}
        },
        {
            {FIELD_INT1,
            {"No Overdraw",
            "No Overdraw (Glow)",
            "Overdraw"}}
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Set_Light_Color, {
        {FIELD_STR1, FIELD_BOOL1},
        {
            {FIELD_STR1, "Light color (str1):"},
            {FIELD_BOOL1, "Random color instead (bool1):"}
        }
    }},
    {AlpineDedEventID::Capture_Point_Handler, {
        {FIELD_STR1, FIELD_FLOAT1, FIELD_FLOAT2, FIELD_INT1, FIELD_INT2, FIELD_BOOL1},
        {
            {FIELD_STR1, "Name (str1):"},
            {FIELD_FLOAT1, "Outline offset (float1):"},
            {FIELD_FLOAT2, "Cap rate multiplier (float2):"},
            {FIELD_INT1, "Stage (used in REV):"},
            {FIELD_INT2, "Position (used in ESC):"},
            {FIELD_BOOL1, "Cylindrical trigger:"}
        },
        {
            {FIELD_INT2,
            {"Basic/Center", "Red base", "Blue base", "Red forward", "Blue forward"}}
        },
        {
            {FIELD_INT2, true}
        }
    }},
    {AlpineDedEventID::Modify_Respawn_Point, {
        {FIELD_BOOL1, FIELD_BOOL2},
        {
            {FIELD_BOOL1, "Red team (bool1):"},
            {FIELD_BOOL2, "Blue team (bool2):"},
        }
    }},
    {AlpineDedEventID::Set_Capture_Point_Owner, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Owner (int1):"},
        },
        {
            {FIELD_INT1,
            {"Neutral", "Red", "Blue"}}
        },
        {
            {FIELD_INT1, true}
        }
    }},
    {AlpineDedEventID::Owner_Gate, {
        {FIELD_INT1, FIELD_INT2},
        {
            {FIELD_INT1, "Handler UID (int1):"},
            {FIELD_INT2, "Required owner (int2):"}
        },
        {
            {FIELD_INT2, {"Neutral", "Red", "Blue"}}
        },
        {
            {FIELD_INT2, true}
        }
    }},
    {AlpineDedEventID::Set_Gameplay_Rule, {
        {FIELD_INT1},
        {
            {FIELD_INT1, "Rule to set (int1):"}
        },
        {
            {FIELD_INT1, {"Player has headlamp"}}
        },
        {
            {FIELD_INT1, true}
        }
    }},
};

void CreateDynamicControls(HWND hwndDlg, const FieldConfig& config, const std::string& className)
{
    HDC hdc = GetDC(hwndDlg); // Get device context to measure font size
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    int controlHeight = tm.tmHeight + 8; // Base control height on font metrics
    int labelWidth = 160;
    int fieldWidth = 160;
    int xLabel = 10;
    int xField = xLabel + labelWidth + 10;
    int yOffset = 10;

    HFONT hDefaultFont = (HFONT)SendMessage(hwndDlg, WM_GETFONT, 0, 0);

    // class name
    {
        std::string label = "Class Name:";
        HWND hLabel = CreateWindowA("STATIC", label.c_str(), WS_VISIBLE | WS_CHILD, xLabel, yOffset + 4, labelWidth,
                                    controlHeight, hwndDlg, nullptr, nullptr, nullptr);

        SendMessage(hLabel, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

        HWND hField = CreateWindowA("STATIC", className.c_str(), WS_VISIBLE | WS_CHILD, xField, yOffset + 4, fieldWidth,
                                    controlHeight, hwndDlg, nullptr, nullptr, nullptr);

        SendMessage(hField, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);
        yOffset += controlHeight + 10; // Increment vertical spacing
    }

    // script name and delay
    std::vector<FieldType> allFields = {FIELD_SCRIPT_NAME, FIELD_DELAY};
    allFields.insert(allFields.end(), config.fieldsToShow.begin(), config.fieldsToShow.end());

    for (FieldType field : allFields) {
        std::string label = config.fieldLabels.count(field) ? config.fieldLabels.at(field) : "";

        if (field == FIELD_SCRIPT_NAME)
            label = "Script Name:";
        if (field == FIELD_DELAY)
            label = "Delay (delay):";

        // Create label
        HWND hLabel = CreateWindowA("STATIC", label.c_str(), WS_VISIBLE | WS_CHILD, xLabel, yOffset + 4, labelWidth,
                                    controlHeight, hwndDlg, nullptr, nullptr, nullptr);

        SendMessage(hLabel, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

        // Create the control
        HWND hField;
        if (config.dropdownItems.count(field)) {
            // Create dropdown for fields with dropdown items
            hField =
                CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_TABSTOP, xField, yOffset,
                              fieldWidth, controlHeight * 5, hwndDlg, reinterpret_cast<HMENU>(field), nullptr, nullptr);

            // Add dropdown items
            const auto& items = config.dropdownItems.at(field);
            for (size_t i = 0; i < items.size(); ++i) {
                std::string itemText;
                if (config.dropdownSaveIndex.count(field) && config.dropdownSaveIndex.at(field)) {
                    // Format as "INDEX : LABEL" when saving index
                    itemText = std::to_string(i) + " : " + items[i];
                }
                else {
                    // Use the label directly otherwise
                    itemText = items[i];
                }
                SendMessage(hField, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(itemText.c_str()));
            }

        }
        else if (field == FIELD_BOOL1 || field == FIELD_BOOL2) {
            // Create checkbox for boolean fields
            hField =
                CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP, xField, yOffset, 20,
                              controlHeight, hwndDlg, reinterpret_cast<HMENU>(field), nullptr, nullptr);
        }
        else {
            // Create edit box for other fields
            hField = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                                   xField, yOffset, fieldWidth, controlHeight, hwndDlg, reinterpret_cast<HMENU>(field),
                                   nullptr, nullptr);
        }

        SendMessage(hField, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

        yOffset += controlHeight + 10;
    }

    // Add OK, Cancel, and Links buttons
    int buttonWidth = 80;
    int buttonHeight = 25;
    int xOK = xLabel;
    int xCancel = xField + fieldWidth - buttonWidth;
    int xLinks = (xOK + xCancel) / 2 - 20; // Adjusted to your original specifications

    HWND hOKButton = CreateWindowW(L"BUTTON", L"Save", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, xOK, yOffset,
                                   buttonWidth, buttonHeight, hwndDlg, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
    SendMessage(hOKButton, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

    HWND hCancelButton =
        CreateWindowW(L"BUTTON", L"Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, xCancel, yOffset,
                      buttonWidth, buttonHeight, hwndDlg, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
    SendMessage(hCancelButton, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

    HWND hLinksButton =
        CreateWindowA("BUTTON", "Save + Edit Links", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, xLinks,
                      yOffset, 120, buttonHeight, hwndDlg, reinterpret_cast<HMENU>(ID_LINKS), nullptr, nullptr);
    SendMessage(hLinksButton, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

    yOffset += buttonHeight + 40;
    ReleaseDC(hwndDlg, hdc);

    // Resize the dialog
    int dialogWidth = xField + fieldWidth + 20;
    int dialogHeight = yOffset;
    SetWindowPos(hwndDlg, HWND_TOPMOST, 0, 0, dialogWidth, dialogHeight, SWP_NOMOVE | SWP_NOZORDER);
}

void SaveCurrentFields(HWND hwndDlg)
{
    if (!currentDedEvent)
        return;

    FieldConfig config = eventFieldConfigs[int_to_af_ded_event(currentDedEvent->event_type)];
    char buffer[256]; // Buffer for dropdown and edit field values

    xlog::info("Saving dialog for event_type: {}", currentDedEvent->event_type);

    // Save mandatory fields
    GetDlgItemTextA(hwndDlg, FIELD_SCRIPT_NAME, buffer, sizeof(buffer));
    currentDedEvent->script_name.assign_0(buffer);
    xlog::info("Saved Script Name: {}", currentDedEvent->script_name.c_str());

    GetDlgItemTextA(hwndDlg, FIELD_DELAY, buffer, sizeof(buffer));
    float delayValue;
    if (is_valid_float(buffer, delayValue)) {
        currentDedEvent->delay = delayValue;
        xlog::info("Saved Delay: {}", currentDedEvent->delay);
    }

    // Save specific fields
    for (FieldType field : config.fieldsToShow) {
        if (config.dropdownItems.count(field)) {
            // Handle dropdown fields
            HWND hDropdown = GetDlgItem(hwndDlg, field);
            int selectedIndex = SendMessage(hDropdown, CB_GETCURSEL, 0, 0); // Get selected index

            if (selectedIndex != CB_ERR) {
                if (config.dropdownSaveIndex.count(field) && config.dropdownSaveIndex.at(field)) {
                    // Save the index directly
                    switch (field) {
                    case FIELD_INT1:
                        currentDedEvent->int1 = selectedIndex;
                        xlog::info("Saved Int1 as index: {}", currentDedEvent->int1);
                        break;
                    case FIELD_INT2:
                        currentDedEvent->int2 = selectedIndex;
                        xlog::info("Saved Int2 as index: {}", currentDedEvent->int2);
                        break;
                    default:
                        break;
                    }
                }
                else {
                    // Save the value based on dropdown item text
                    SendMessage(hDropdown, CB_GETLBTEXT, selectedIndex, (LPARAM)buffer);
                    switch (field) {
                    case FIELD_INT1:
                        currentDedEvent->int1 = std::stoi(buffer);
                        xlog::info("Saved Int1 as value: {}", currentDedEvent->int1);
                        break;
                    case FIELD_INT2:
                        currentDedEvent->int2 = std::stoi(buffer);
                        xlog::info("Saved Int2 as value: {}", currentDedEvent->int2);
                        break;
                    case FIELD_STR1:
                        currentDedEvent->str1.assign_0(buffer);
                        xlog::info("Saved Str1: {}", currentDedEvent->str1.c_str());
                        break;
                    case FIELD_STR2:
                        currentDedEvent->str2.assign_0(buffer);
                        xlog::info("Saved Str2: {}", currentDedEvent->str2.c_str());
                        break;
                    default:
                        break;
                    }
                }
            }

        }
        else {
            // Handle non-dropdown fields
            switch (field) {
            case FIELD_INT1:
                currentDedEvent->int1 = GetDlgItemInt(hwndDlg, FIELD_INT1, nullptr, TRUE);
                xlog::info("Saved Int1: {}", currentDedEvent->int1);
                break;
            case FIELD_INT2:
                currentDedEvent->int2 = GetDlgItemInt(hwndDlg, FIELD_INT2, nullptr, TRUE);
                xlog::info("Saved Int2: {}", currentDedEvent->int2);
                break;
            case FIELD_FLOAT1:
                GetDlgItemTextA(hwndDlg, FIELD_FLOAT1, buffer, sizeof(buffer));
                if (is_valid_float(buffer, delayValue)) {
                    currentDedEvent->float1 = delayValue;
                    xlog::info("Saved Float1: {}", currentDedEvent->float1);
                }
                break;
            case FIELD_FLOAT2:
                GetDlgItemTextA(hwndDlg, FIELD_FLOAT2, buffer, sizeof(buffer));
                if (is_valid_float(buffer, delayValue)) {
                    currentDedEvent->float2 = delayValue;
                    xlog::info("Saved Float2: {}", currentDedEvent->float2);
                }
                break;
            case FIELD_BOOL1:
                currentDedEvent->bool1 = IsDlgButtonChecked(hwndDlg, FIELD_BOOL1) == BST_CHECKED;
                xlog::info("Saved Bool1: {}", currentDedEvent->bool1);
                break;
            case FIELD_BOOL2:
                currentDedEvent->bool2 = IsDlgButtonChecked(hwndDlg, FIELD_BOOL2) == BST_CHECKED;
                xlog::info("Saved Bool2: {}", currentDedEvent->bool2);
                break;
            case FIELD_STR1:
                GetDlgItemTextA(hwndDlg, FIELD_STR1, buffer, sizeof(buffer));
                currentDedEvent->str1.assign_0(buffer);
                xlog::info("Saved Str1: {}", currentDedEvent->str1.c_str());
                break;
            case FIELD_STR2:
                GetDlgItemTextA(hwndDlg, FIELD_STR2, buffer, sizeof(buffer));
                currentDedEvent->str2.assign_0(buffer);
                xlog::info("Saved Str2: {}", currentDedEvent->str2.c_str());
                break;
            default:
                break;
            }
        }
    }
}

INT_PTR CALLBACK DialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_INITDIALOG: {
        if (currentDedEvent) {
            FieldConfig config = eventFieldConfigs[int_to_af_ded_event(currentDedEvent->event_type)];

            xlog::info("Opening dialog for event_type: {}", currentDedEvent->event_type);

            // Set dialog title
            std::string title = generate_window_title(currentDedEvent);
            SetWindowTextA(hwndDlg, title.c_str());

            // Create dynamic controls (already populates dropdowns)
            CreateDynamicControls(hwndDlg, config, currentDedEvent->class_name.c_str());

            // Populate mandatory fields
            SetDlgItemTextA(hwndDlg, FIELD_SCRIPT_NAME, currentDedEvent->script_name.c_str());
            SetDlgItemTextA(hwndDlg, FIELD_DELAY, std::to_string(currentDedEvent->delay).c_str());

            // Populate fields, including dropdowns
            for (FieldType field : config.fieldsToShow) {
                HWND hField = GetDlgItem(hwndDlg, field);
                if (!hField)
                    continue; // Skip if control does not exist

                if (config.dropdownItems.count(field)) {
                    // Handle dropdowns
                    if (config.dropdownSaveIndex.count(field) && config.dropdownSaveIndex.at(field)) {
                        // Use index to populate dropdown selection
                        int currentIndex = 0;
                        switch (field) {
                        case FIELD_INT1:
                            currentIndex = currentDedEvent->int1;
                            break;
                        case FIELD_INT2:
                            currentIndex = currentDedEvent->int2;
                            break;
                        default:
                            continue;
                        }
                        SendMessage(hField, CB_SETCURSEL, currentIndex, 0);
                    }
                    else {
                        // Match value to dropdown item
                        std::string currentValue;
                        switch (field) {
                        case FIELD_INT1:
                            currentValue = std::to_string(currentDedEvent->int1);
                            break;
                        case FIELD_INT2:
                            currentValue = std::to_string(currentDedEvent->int2);
                            break;
                        case FIELD_STR1:
                            currentValue = currentDedEvent->str1.c_str();
                            break;
                        case FIELD_STR2:
                            currentValue = currentDedEvent->str2.c_str();
                            break;
                        default:
                            continue;
                        }

                        // Find and select the index in the dropdown that matches the current value
                        int index = SendMessage(hField, CB_FINDSTRINGEXACT, -1, (LPARAM)currentValue.c_str());
                        if (index != CB_ERR) {
                            SendMessage(hField, CB_SETCURSEL, index, 0);
                        }
                        else {
                            SendMessage(hField, CB_SETCURSEL, 0, 0); // Default to the first item
                        }
                    }

                }
                else {
                    // Handle non-dropdown fields
                    switch (field) {
                    case FIELD_INT1:
                        SetDlgItemInt(hwndDlg, FIELD_INT1, currentDedEvent->int1, TRUE);
                        break;
                    case FIELD_INT2:
                        SetDlgItemInt(hwndDlg, FIELD_INT2, currentDedEvent->int2, TRUE);
                        break;
                    case FIELD_FLOAT1:
                        SetDlgItemTextA(hwndDlg, FIELD_FLOAT1, std::to_string(currentDedEvent->float1).c_str());
                        break;
                    case FIELD_FLOAT2:
                        SetDlgItemTextA(hwndDlg, FIELD_FLOAT2, std::to_string(currentDedEvent->float2).c_str());
                        break;
                    case FIELD_BOOL1:
                        CheckDlgButton(hwndDlg, FIELD_BOOL1, currentDedEvent->bool1 ? BST_CHECKED : BST_UNCHECKED);
                        break;
                    case FIELD_BOOL2:
                        CheckDlgButton(hwndDlg, FIELD_BOOL2, currentDedEvent->bool2 ? BST_CHECKED : BST_UNCHECKED);
                        break;
                    case FIELD_STR1:
                        SetDlgItemTextA(hwndDlg, FIELD_STR1, currentDedEvent->str1.c_str());
                        break;
                    case FIELD_STR2:
                        SetDlgItemTextA(hwndDlg, FIELD_STR2, currentDedEvent->str2.c_str());
                        break;
                    default:
                        break;
                    }
                }
            }
        }
        return TRUE;
    }

    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK && currentDedEvent)
        {
            SaveCurrentFields(hwndDlg);
            EndDialog(hwndDlg, IDOK);
            return TRUE;
        }
        else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwndDlg, IDCANCEL);
            return TRUE;
        }
        else if (LOWORD(wParam) == ID_LINKS && currentDedEvent) {
            // Save current fields before opening Links dialog
            SaveCurrentFields(hwndDlg);
            EndDialog(hwndDlg, IDOK);

            if (currentDedLevel) {
                xlog::info("Opening Links dialog...");
                OpenLinksDialog(currentDedLevel);
            }
            return TRUE;
        }

        break;
    }

    default:
        return FALSE;
    }
    return FALSE;
}

bool ShowAlpineEventDialog(HWND parent, DedEvent* dedEvent, CDedLevel* level)
{
    currentDedEvent = dedEvent;
    currentDedLevel = level;

    // Use DialogBoxParam for modal behavior
    int result =
        DialogBoxParam(((HINSTANCE) & __ImageBase), MAKEINTRESOURCE(IDD_ALPINE_EVENT_PROPERTIES), parent, DialogProc, 0);

    if (result == -1) {
        DWORD error = GetLastError();
        xlog::error("DialogBox failed with error code: {}", error);
    }

    // Reset after dialog ends
    currentDedEvent = nullptr;
    currentDedLevel = nullptr;

    return (result == IDOK);
}

void OpenAlpineEventProperties(DedEvent* dedEvent, CDedLevel* level) {
    if (ShowAlpineEventDialog(GetActiveWindow(), dedEvent, level)) {
        xlog::info("Dialog saved successfully!");
    }
    else {
        xlog::warn("Dialog canceled.");
    }
}

// set template, in CDedLevel__OpenEventProperties
CodeInjection open_event_properties_patch{
    0x00408D6D, [](auto& regs) {
        CDedLevel* level = regs.edi;
        DedEvent* event = regs.ebx;
        //xlog::warn("opening properties on event {}, type {}", event->class_name.c_str(), event->event_type);
        //xlog::warn("selection is {}", level->selection[0]);

        if (event->event_type > 88) { // only redirect alpine events
            OpenAlpineEventProperties(event, level);
            regs.eip = 0x00408F34;
        }
    }
};

// directional events
CodeInjection DedEvent__exchange_patch {
    0x00451614, [](auto& regs) {

        DedEvent* event = regs.esi;
        VFile* file = regs.edi;

        if (event->event_type == static_cast<int>(AlpineDedEventID::AF_Teleport_Player) ||
            event->event_type == static_cast<int>(AlpineDedEventID::Clone_Entity) ||
            event->event_type == static_cast<int>(AlpineDedEventID::Anchor_Marker_Orient)) {

            // save orientation to rfl file
            // NOTE: the game MUST read this properly or very bad things will happen
            Matrix3* mat = &event->orient;
            file->rw_matrix(mat, 300, &editor_file_default_matrix);
            
            regs.edx = mat;
            regs.eip = 0x0045165C;
        }
    }
};

// 3d arrows
CodeInjection arrows_for_events_patch {
    0x00421654, [](auto& regs) {

        int event_type = *reinterpret_cast<int*>(regs.esi + 0x94);

        // Arrow is drawn for Teleport_Player as well (stock game behaviour)
        if (event_type == 41 || // Play_VClip
            event_type == 69 || // Teleport
            event_type == static_cast<int>(AlpineDedEventID::AF_Teleport_Player) ||
            event_type == static_cast<int>(AlpineDedEventID::Clone_Entity) ||
            event_type == static_cast<int>(AlpineDedEventID::Anchor_Marker_Orient)) {
            regs.eip = 0x00421661; // draw 3d arrow
        }
    }
};

void ApplyEventsPatches()
{
    // Support custom events with orientation
    DedEvent__exchange_patch.install();

    // Render 3d arrows for events that save orientation
    arrows_for_events_patch.install();

    // Support custom event integration
    initialize_event_names(); // populate extended array with stock + AF events
    // debug_event_names(); // debug logging

    // assign extended events array
    OnInitDialog_redirect_event_names.install();   // replace reference to event_names with new extended array
    get_event_type_redirect_event_names.install(); // replace reference to event_names with new extended array
    event_names_injection.install(); // when opening event properties, use new extended array for event look up

    // handle event properties windows for AF events
    open_event_properties_patch.install(); // redirect AF events to new properties dialog

    // set new end address for event array loops that use new extended array
    AsmWriter(0x004617FC) // OnInitDialog
        .cmp(asm_regs::edi, reinterpret_cast<uintptr_t>(&extended_event_names[total_event_count - 1]));
    AsmWriter(0x004516C2) // get_event_type_from_class_name
        .cmp(asm_regs::esi, reinterpret_cast<uintptr_t>(&extended_event_names[total_event_count - 1]));

    // verify_event_names(); // debug logging

    // Make Set_Liquid_Depth show in events panel, Original code omits that event by name
    AsmWriter(0x004440B4).push("_dummy");
}
