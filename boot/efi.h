/* efi.h - the subset of UEFI 2.x that the KestrelOS loader actually uses.
 *
 * Written from the specification rather than pulled in from gnu-efi/EDK2 so the
 * loader has no external dependency and builds with nothing but clang + lld.
 * All firmware entry points use the Microsoft x64 calling convention, which is
 * what --target=x86_64-unknown-windows gives us by default, so no per-call
 * annotation is needed.
 */
#ifndef KESTREL_EFI_H
#define KESTREL_EFI_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   BOOLEAN;
typedef int64_t   INTN;
typedef uint64_t  UINTN;
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef char      CHAR8;
typedef uint16_t  CHAR16;
typedef void      VOID;
typedef UINTN     EFI_STATUS;
typedef VOID     *EFI_HANDLE;
typedef VOID     *EFI_EVENT;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;
typedef UINT64    EFI_LBA;

#define EFIAPI
#define IN
#define OUT
#define OPTIONAL

#define EFI_ERROR(s)                (((INTN)(s)) < 0)
#define EFI_ERR(n)                  ((EFI_STATUS)(0x8000000000000000ULL | (n)))
#define EFI_SUCCESS                 0
#define EFI_LOAD_ERROR              EFI_ERR(1)
#define EFI_INVALID_PARAMETER       EFI_ERR(2)
#define EFI_UNSUPPORTED             EFI_ERR(3)
#define EFI_BAD_BUFFER_SIZE         EFI_ERR(4)
#define EFI_BUFFER_TOO_SMALL        EFI_ERR(5)
#define EFI_NOT_READY               EFI_ERR(6)
#define EFI_DEVICE_ERROR            EFI_ERR(7)
#define EFI_WRITE_PROTECTED         EFI_ERR(8)
#define EFI_OUT_OF_RESOURCES        EFI_ERR(9)
#define EFI_NOT_FOUND               EFI_ERR(14)
#define EFI_ABORTED                 EFI_ERR(21)
#define EFI_SECURITY_VIOLATION      EFI_ERR(26)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* ------------------------------------------------------------------ GUIDs */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x0964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_FILE_INFO_GUID \
    {0x09576E92,0x6D3F,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}}
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    {0x964E5B21,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    {0x09576E91,0x6D3F,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID \
    {0xDD9E7534,0x7762,0x4698,{0x8C,0x14,0xF5,0x85,0x17,0xA6,0x25,0xAA}}
#define ACPI_20_TABLE_GUID \
    {0x8868E871,0xE4F1,0x11D3,{0xBC,0x22,0x00,0x80,0xC7,0x3C,0x88,0x81}}
#define ACPI_10_TABLE_GUID \
    {0xEB9D2D30,0x2D88,0x11D3,{0x9A,0x16,0x00,0x90,0x27,0x3F,0xC1,0x4D}}
#define EFI_GLOBAL_VARIABLE_GUID \
    {0x8BE4DF61,0x93CA,0x11D2,{0xAA,0x0D,0x00,0xE0,0x98,0x03,0x2B,0x8C}}

/* --------------------------------------------------------- text in/output */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    INT32   MaxMode;
    INT32   Mode;
    INT32   Attribute;
    INT32   CursorColumn;
    INT32   CursorRow;
    BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Extended);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    EFI_STATUS (EFIAPI *TestString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    EFI_STATUS (EFIAPI *QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode, UINTN *Cols, UINTN *Rows);
    EFI_STATUS (EFIAPI *SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode);
    EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    EFI_STATUS (EFIAPI *SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Col, UINTN Row);
    EFI_STATUS (EFIAPI *EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Enable);
    SIMPLE_TEXT_OUTPUT_MODE *Mode;
};

#define EFI_BLACK        0x00
#define EFI_LIGHTGRAY    0x07
#define EFI_DARKGRAY     0x08
#define EFI_LIGHTRED     0x0C
#define EFI_YELLOW       0x0E
#define EFI_WHITE        0x0F
#define EFI_BACKGROUND_BLUE 0x10

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

#define SCAN_UP     0x01
#define SCAN_DOWN   0x02
#define SCAN_ESC    0x17

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Extended);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
};

/* ------------------------------------------------------------- memory map */
typedef enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
    EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
    EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory, EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;

typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

/* ------------------------------------------------------------- file system */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_DIRECTORY   0x0000000000000010ULL

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    UINT8  CreateTime[16];
    UINT8  LastAccessTime[16];
    UINT8  ModificationTime[16];
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *This, UINT64 *Pos);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, UINT64 Pos);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN *Size, VOID *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN Size, VOID *Buffer);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *This);
};

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
};

/* ----------------------------------------------------------- graphics out */
typedef struct {
    UINT32 RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Mode, UINTN *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Mode);
    VOID       *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* -------------------------------------------------------------- block i/o */
typedef struct {
    UINT32  MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32  BlockSize;
    UINT32  IoAlign;
    EFI_LBA LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;
struct _EFI_BLOCK_IO_PROTOCOL {
    UINT64              Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (EFIAPI *Reset)(EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended);
    EFI_STATUS (EFIAPI *ReadBlocks)(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buf);
    EFI_STATUS (EFIAPI *WriteBlocks)(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buf);
    EFI_STATUS (EFIAPI *FlushBlocks)(EFI_BLOCK_IO_PROTOCOL *This);
};

/* ------------------------------------------------------------ device path */
typedef struct {
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#define MEDIA_DEVICE_PATH        0x04
#define MEDIA_HARDDRIVE_DP       0x01
#define END_DEVICE_PATH_TYPE     0x7F

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;
    UINT32 PartitionNumber;
    UINT64 PartitionStart;
    UINT64 PartitionSize;
    UINT8  Signature[16];
    UINT8  MBRType;
    UINT8  SignatureType;
} HARDDRIVE_DEVICE_PATH;

/* ------------------------------------------------------------ loaded image */
typedef struct {
    UINT32                    Revision;
    EFI_HANDLE                ParentHandle;
    VOID                     *SystemTable;
    EFI_HANDLE                DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL *FilePath;
    VOID                     *Reserved;
    UINT32                    LoadOptionsSize;
    VOID                     *LoadOptions;
    VOID                     *ImageBase;
    UINT64                    ImageSize;
    EFI_MEMORY_TYPE           ImageCodeType;
    EFI_MEMORY_TYPE           ImageDataType;
    VOID                     *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* -------------------------------------------------------------- services */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;
typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;

typedef struct {
    EFI_TABLE_HEADER Hdr;

    VOID *RaiseTPL;
    VOID *RestoreTPL;

    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MapSize, EFI_MEMORY_DESCRIPTOR *Map, UINTN *MapKey, UINTN *DescSize, UINT32 *DescVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(VOID *Buffer);

    EFI_STATUS (EFIAPI *CreateEvent)(UINT32 Type, UINTN Tpl, VOID *Fn, VOID *Ctx, EFI_EVENT *Event);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 TriggerTime);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    VOID *SignalEvent;
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
    VOID *CheckEvent;

    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    EFI_STATUS (EFIAPI *LocateHandle)(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Protocol, VOID *Key, UINTN *BufSize, EFI_HANDLE *Buf);
    EFI_STATUS (EFIAPI *LocateDevicePath)(EFI_GUID *Protocol, EFI_DEVICE_PATH_PROTOCOL **Path, EFI_HANDLE *Device);
    VOID *InstallConfigurationTable;

    VOID *LoadImage;
    VOID *StartImage;
    VOID (EFIAPI *Exit)(EFI_HANDLE Image, EFI_STATUS Status, UINTN DataSize, CHAR16 *Data);
    VOID *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE Image, UINTN MapKey);

    EFI_STATUS (EFIAPI *GetNextMonotonicCount)(UINT64 *Count);
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 Code, UINTN DataSize, CHAR16 *Data);

    VOID *ConnectController;
    VOID *DisconnectController;

    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface, EFI_HANDLE AgentHandle,
                                      EFI_HANDLE ControllerHandle, UINT32 Attributes);
    EFI_STATUS (EFIAPI *CloseProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, EFI_HANDLE Agent, EFI_HANDLE Controller);
    VOID *OpenProtocolInformation;

    VOID *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Protocol, VOID *Key, UINTN *NoHandles, EFI_HANDLE **Buf);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;

    VOID *CalculateCrc32;
    VOID (EFIAPI *CopyMem)(VOID *Dest, VOID *Src, UINTN Len);
    VOID (EFIAPI *SetMem)(VOID *Buffer, UINTN Size, UINT8 Value);
    VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    UINT16 Year;
    UINT8  Month, Day, Hour, Minute, Second, Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight, Pad2;
} EFI_TIME;

#define EFI_VARIABLE_NON_VOLATILE        0x01
#define EFI_VARIABLE_BOOTSERVICE_ACCESS  0x02
#define EFI_VARIABLE_RUNTIME_ACCESS      0x04

#define EFI_RESET_COLD 0
#define EFI_RESET_WARM 1
#define EFI_RESET_SHUTDOWN 2

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *Time, VOID *Caps);
    EFI_STATUS (EFIAPI *SetTime)(EFI_TIME *Time);
    VOID *GetWakeupTime;
    VOID *SetWakeupTime;
    EFI_STATUS (EFIAPI *SetVirtualAddressMap)(UINTN MapSize, UINTN DescSize, UINT32 DescVersion, EFI_MEMORY_DESCRIPTOR *Map);
    VOID *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *Name, EFI_GUID *Vendor, UINT32 *Attr, UINTN *Size, VOID *Data);
    EFI_STATUS (EFIAPI *GetNextVariableName)(UINTN *NameSize, CHAR16 *Name, EFI_GUID *Vendor);
    EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *Name, EFI_GUID *Vendor, UINT32 Attr, UINTN Size, VOID *Data);
    VOID *GetNextHighMonotonicCount;
    VOID (EFIAPI *ResetSystem)(UINT32 Type, EFI_STATUS Status, UINTN DataSize, VOID *Data);
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_GUID VendorGuid;
    VOID    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* KESTREL_EFI_H */
