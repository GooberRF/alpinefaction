#pragma once

#include <windows.h>
#include <patch_common/MemUtils.h>
#include <mbstring.h>
#include <algorithm>

extern "C" IMAGE_DOS_HEADER __ImageBase;

struct CWnd_mbrs
{
    char padding1[0x18];
    HWND m_hWnd; // window handle
    char padding2[0x1C];
};
static_assert(sizeof(CWnd_mbrs) == 0x38, "CWnd_mbrs size mismatch!");

struct CWnd
{
    int _vft;
    CWnd_mbrs _d;

    int DedMessageBox(LPCSTR lpText, LPCSTR lpCaption, UINT uType)
    {
        return AddrCaller{0x00531BBA}.this_call<int>(this, lpText, lpCaption, uType);
    }
};
static_assert(sizeof(CWnd) == 0x3C, "CWnd size mismatch!");

struct CComboBox : public CWnd
{
    int AddString(const char* lpszString)
    {
        return static_cast<int>(SendMessageA(this->_d.m_hWnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lpszString)));
    }
};

struct CDataExchange
{
	BOOL m_bSaveAndValidate;
	CWnd* m_pDlgWnd;
	HWND m_hWndLastControl;
	BOOL m_bEditLastControl;
};

struct CString
{
    char* m_pchData;

    CString() : m_pchData(nullptr) {}

    CString(const char* s)
    {
        assign(s);
    }

    ~CString()
    {
        delete[] m_pchData;
    }

    void assign(const char* s)
    {
        delete[] m_pchData;
        if (s) {
            size_t len = strlen(s) + 1;
            m_pchData = new char[len];
            strcpy(m_pchData, s);
        }
        else {
            m_pchData = nullptr;
        }
    }

    bool empty() const
    {
        return m_pchData == nullptr || m_pchData[0] == '\0';
    }

    const char* c_str() const
    {
        return m_pchData ? m_pchData : "";
    }

    CString& operator=(const char* Dst)
    {
        static AddrCaller operatorCaller{0x0052FBDC};
        operatorCaller.this_call<void>(this, Dst);
        return *this;
    }

    operator const char*() const
    {
        return m_pchData ? m_pchData : "";
    }

    void* GetBuffer(int bufferSize)
    {
        // Allocate a new buffer
        delete[] m_pchData;
        m_pchData = new char[bufferSize + 1];
        return m_pchData;
    }

    void ReleaseBuffer(int newLength)
    {
        if (newLength == -1) {
            newLength = strlen(m_pchData);
        }
        m_pchData[newLength] = '\0';
    }
};

inline HWND WndToHandle(CWnd* wnd)
{
    return struct_field_ref<HWND>(wnd, 4 + 0x18);
}

// Note object IDs in editor are different from game
enum class DedObjectType : int
{
    DED_CLUTTER = 0x0,
    DED_ENTITY = 0x1,
    DED_ITEM = 0x2,
    DED_RESPAWN_POINT = 0x3,
    DED_TRIGGER = 0x4,
    DED_AMBIENT_SOUND = 0x5,
    DED_LIGHT = 0x7,
    DED_GEO_REGION = 0x8,
    DED_NAV_POINT = 0x9,
    DED_EVENT = 0xA,
    DED_CUTSCENE_CAMERA = 0xB,
    DED_CUTSCENE_PATH_NODE = 0xC,
    DED_PARTICLE_EMITTER = 0xD,
    DED_GAS_REGION = 0xE,
    DED_ROOM_EFFECT = 0xF,
    DED_EAX_EFFECT = 0x10,
    DED_CLIMBING_REGION = 0x11,
    DED_DECAL = 0x12,
    DED_BOLT_EMITTER = 0x13,
    DED_TARGET = 0x14,
    DED_KEYFRAME = 0x15,
    DED_PUSH_REGION = 0x16
};

struct Vector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector3() = default;
    Vector3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};
static_assert(sizeof(Vector3) == 0xC, "Vector3 size mismatch!");


struct Matrix3
{
    Vector3 rvec;
    Vector3 uvec;
    Vector3 fvec;

    Matrix3() = default;
    Matrix3(const Vector3& r, const Vector3& u, const Vector3& f) : rvec(r), uvec(u), fvec(f) {}
};
static_assert(sizeof(Matrix3) == 0x24, "Matrix3 size mismatch!");

template<typename T>
struct VArray
{
    int size;
    int capacity;
    T* data_ptr;

    T& operator[](size_t index)
    {
        if (index >= static_cast<size_t>(size)) {
            throw std::out_of_range("Index out of range");
        }
        return data_ptr[index];
    }

    const T& operator[](size_t index) const
    {
        if (index >= static_cast<size_t>(size)) {
            throw std::out_of_range("Index out of range");
        }
        return data_ptr[index];
    }

    int get_size() const
    {
        return size;
    }

    int get_capacity() const
    {
        return capacity;
    }

    int add_if_not_exists_raw(void* value)
    {
        return AddrCaller{0x004127E0}.this_call<int>(this, value);
    }

    int add_if_not_exists_int(int value)
    {
        return add_if_not_exists_raw(reinterpret_cast<void*>(value));
    }
};
static_assert(sizeof(VArray<int>) == 0xC, "VArray size mismatch!");

struct VString
{
    int max_len; // Maximum length of the string
    char* buf;   // Pointer to the character buffer

    // Default constructor: initializes an empty string
    VString() : max_len(0), buf(nullptr) {}

    // Original "assign" function that copies another VString
    VString& assign(const VString& other)
    {
        static AddrCaller assignCaller{0x004B6DB0}; // Address of String::assign
        assignCaller.this_call<int>(this, &other);
        return *this;
    }

    // Original "assign_0" function that assigns a const char*
    VString& assign_0(const char* str)
    {
        static AddrCaller assign0Caller{0x004B6E10}; // Address of String::assign_0
        assign0Caller.this_call<int>(this, str);
        return *this;
    }

    // Original "cstr" function to get the buffer as a C-style string
    const char* cstr()
    {
        static AddrCaller cstrCaller{0x004B6810};
        return cstrCaller.this_call<const char*>(this);
    }

    // Fallback method to access the string as a C-style string
    const char* c_str() const
    {
        return buf ? buf : "";
    }

    // Check if the string is empty
    bool empty() const
    {
        return buf == nullptr || buf[0] == '\0';
    }

    // Prevent copy semantics to avoid double deletion
    VString(const VString&) = delete;
    VString& operator=(const VString&) = delete;

    // Allow move semantics
    VString(VString&& other) noexcept : max_len(other.max_len), buf(other.buf)
    {
        other.max_len = 0;
        other.buf = nullptr;
    }
};
static_assert(sizeof(VString) == 0x8, "VString size mismatch!");

struct Color
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;

    // default
    Color() : r(0), g(0), b(0), a(255) {}

    // constructor
    Color(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255) : r(red), g(green), b(blue), a(alpha) {}
};
static_assert(sizeof(Color) == 0x4, "Color size mismatch!");

struct DedObject
{
    void* vtbl;
    VString field_4;
    void* vmesh;
    int field_10;
    Vector3 pos;
    Matrix3 orient;
    VString script_name;
    int uid;
    VString class_name;
    bool hidden_in_editor;
    char padding1[3];
    DedObjectType type;
    Vector3 field_60;
    Vector3 field_6C;
    char field_78;
    char padding2[3];
    VArray<int> links;
    VArray<std::string> field_88;
};
static_assert(sizeof(DedObject) == 0x94, "DedObject size mismatch!");


struct DedEvent : DedObject
{
    //DedObject baseclass_0;
    int event_type;
    float delay;
    Color color;
    int int1;
    int int2;
    float float1;
    float float2;
    bool bool1;
    bool bool2;
    char padding[2];
    VString str1;
    VString str2;
};
static_assert(sizeof(DedEvent) == 0xC4, "DedEvent size mismatch");


struct CDialog_mbrs
{
    char padding[0x58]; // placeholder
};

struct CDialog
{
    void* _vft;
    CDialog_mbrs _d;
};
static_assert(sizeof(CDialog) == 0x5C, "CDialog size mismatch!");

struct CEdit : CWnd
{
    // placeholder
};
static_assert(sizeof(CEdit) == 0x3C, "CEdit size mismatch!");

struct CStatic : CWnd
{
    // placeholder
};
static_assert(sizeof(CStatic) == 0x3C, "CStatic size mismatch!");

struct CButton : CWnd
{
    // placeholder
};
static_assert(sizeof(CButton) == 0x3C, "CButton size mismatch!");

// can probably (hopefully) be deleted now, waiting to confirm
struct CEventDialog : CDialog
{
    // Base class fields
    CEdit field_5C;
    CString field_98;
    CString field_9C; // Script name field
    CStatic field_A0;
    CWnd field_DC;
    CString field_118; // Delay field
    CStatic field_11C;
    CComboBox field_158;
    CString field_194; // Class name field
    CButton field_198;
    CStatic field_1D4;
    CComboBox field_210;
    CString field_24C;
    CEdit field_250;
    CStatic field_28C;
    CString field_2C8;
    CEdit field_2CC;
    CStatic field_308;
    CString field_344;
    int field_348;
    CStatic field_34C;
    CWnd field_388;
    CString field_3C4;
    CStatic field_3C8;
    CWnd field_404;
    CString field_440;
    CStatic field_444;
    CEdit field_480;
    CString field_4BC;
    CStatic field_4C0;
    CWnd field_4FC;
    CString field_538;
    CStatic field_53C;
    CWnd field_578;
    CString field_5B4;
    CStatic field_5B8;
    CEdit field_5F4;
    CString field_630;
    int field_634;
    CStatic field_638;
    CComboBox field_674;
    CString field_6B0;
    CButton field_6B4;
    int field_6F0;
    int field_6F4;
    CStatic field_6F8;
    CEdit field_734;
    CString field_770;
    CStatic field_774;
    CEdit field_7B0;
    CString field_7EC;
    int field_7F0;
    CStatic field_7F4;
    CEdit field_830;
    CString field_86C;
    int field_870;
    CStatic field_874;
    CEdit field_8B0;
    CString field_8EC;
    CStatic field_8F0;
    CComboBox field_92C;
    int field_968;
    CStatic field_96C;
    CEdit field_9A8;
    CString field_9E4;
    CStatic field_9E8;
    CEdit field_A24;
    CEdit field_A60;
    int field_A9C;
    CStatic field_AA0;
    CEdit field_ADC;
    CString field_B18;
    CStatic field_B1C;
    CComboBox field_B58;
    int field_B94;
    CStatic field_B98;
    CWnd field_BD4;
    CString field_C10;
    CStatic field_C14;
    CWnd field_C50;
    CString field_C8C;
    CStatic field_C90;
    CComboBox field_CCC;
    CString field_D08;
    CStatic field_D0C;
    CWnd field_D48;
    CString field_D84;
    CStatic field_D88;
    CEdit field_DC4;
    CString field_E00;
    int field_E04;
    CStatic field_E08;
    CEdit field_E44;
    CString field_E80;
    CStatic field_E84;
    CEdit field_EC0;
    CString field_EFC; // template 222 as str1
    int field_F00;     // template 222 as bool1
    CStatic field_F04;
    CComboBox field_F40;
    CString field_F7C;
    CStatic field_F80;
    CComboBox field_FBC;
    CString field_FF8;
    CStatic field_FFC;
    CWnd field_1038;
    CString field_1074;
    int field_1078;
    CStatic field_107C;
    CEdit field_10B8;
    CString field_10F4;
    CStatic field_10F8;
    CComboBox field_1134;
    CString field_1170;
    int field_1174;
    int field_1178;
    CStatic field_117C;
    CComboBox field_11B8;
    int field_11F4;
    CStatic field_11F8;
    CComboBox field_1234;
    int field_1270;
    CStatic field_1274;
    CComboBox field_12B0;
    int field_12EC;
    CStatic field_12F0;
    CComboBox field_132C;
    int field_1368;
    CStatic field_136C;
    CComboBox field_13A8;
    int field_13E4;
    CStatic field_13E8;
    CEdit field_1424;
    CString field_1460;
    CStatic field_1464;
    CEdit field_14A0;
    int field_14DC;
    int field_14E0;
    int field_14E4;
    int field_14E8;
    int field_14EC;
    int field_14F0;
    int field_14F4; // used as bool1 in template 291
    CStatic field_14F8[2];
    CEdit field_1570[2];
    CString field_15E8[2];
    CStatic field_15F0[2];
    CEdit field_1668[2];
    CString field_16E0[2];
    CStatic field_16E8;
    char field_1724[3264]; // Large array field, treat as a CString starting at specified offset
    CString field_23E4;
    char field_23E8;
    char field_23E9;
    char padding1[2];
    int field_23EC; // stores template id
    int field_23F0;
    int field_23F4;
    int field_23F8;
};
static_assert(sizeof(CEventDialog) == 0x23FC, "CEventDialog size mismatch!");
static_assert(offsetof(CEventDialog, field_5C) == 0x5C, "field_5C offset mismatch!");
static_assert(offsetof(CEventDialog, field_98) == 0x98, "field_98 offset mismatch!");
static_assert(offsetof(CEventDialog, field_1724) == 0x1724, "field_1724 offset mismatch!");

struct CCmdTarget_mbrs
{
    char padding[0x18];
};
static_assert(sizeof(CCmdTarget_mbrs) == 0x18);

struct CDocument_mbrs : CCmdTarget_mbrs
{
    CString m_strTitle;
    CString m_strPathName;
    void* m_pDocTemplate;
    char m_viewList[0x1C]; // CPtrList
    BOOL m_bModified;
    BOOL m_bAutoDelete;
    BOOL m_bEmbedded;
};
static_assert(sizeof(CDocument_mbrs) == 0x4C);

struct CDocument
{
    void* _vft;
    CDocument_mbrs _d;

    CString GetPathName()
    {
        return AddrCaller{0x0044A520}.this_call<CString>(this);
    }
};
static_assert(sizeof(CDocument) == 0x50);

struct VFile
{
    int DirId;
    char bExists;
    char szFilename[259];
    void* pPackfileEntry;
    int bCdFile;
    int open_file_index;

    void rw_matrix(Matrix3* mat, int ver, Matrix3* deflt)
    {
        AddrCaller{0x004D1820}.this_call(this, mat, ver, deflt);
    }

    int get_version()
    {
        AddrCaller{0x004CF680}.this_call(this);
    }
};
static_assert(sizeof(VFile) == 0x114);

bool get_is_saving_af_version();

static auto& editor_file_default_matrix = *reinterpret_cast<Matrix3*>(0x01642060);
static auto& g_main_frame = addr_as_ref<CWnd*>(0x006F9E68);
