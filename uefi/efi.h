/* Just enough UEFI to load a kernel and get out of the firmware's way.
 *
 * This is not a general binding: every structure stops at the last field the
 * stub actually uses, and the members before it are placeholders holding their
 * offsets.  Nothing here is ever allocated by us -- the firmware hands out all
 * of it -- so the tails being absent costs nothing.
 *
 * Firmware functions use the Microsoft x64 calling convention rather than the
 * SysV one everything else is compiled for, hence EFI_ABI on each of them.
 */
#ifndef WOS_UEFI_EFI_H
#define WOS_UEFI_EFI_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

typedef uint64_t efi_status_t;
typedef void    *efi_handle_t;

#define EFI_SUCCESS  0
#define EFI_ERROR(s) (((efi_status_t)(s)) >> 63)

#define EFI_ABI __attribute__((ms_abi))

/* EFI_LOCATE_SEARCH_TYPE */
#define BY_PROTOCOL 2

/* EFI_ALLOCATE_TYPE */
#define ALLOCATE_ANY_PAGES   0
#define ALLOCATE_MAX_ADDRESS 1
#define ALLOCATE_ADDRESS     2

/* EFI_MEMORY_TYPE */
#define EFI_RESERVED_MEMORY       0
#define EFI_LOADER_CODE           1
#define EFI_LOADER_DATA           2
#define EFI_BOOT_SERVICES_CODE    3
#define EFI_BOOT_SERVICES_DATA    4
#define EFI_RUNTIME_SERVICES_CODE 5
#define EFI_RUNTIME_SERVICES_DATA 6
#define EFI_CONVENTIONAL_MEMORY   7
#define EFI_UNUSABLE_MEMORY       8
#define EFI_ACPI_RECLAIM_MEMORY   9
#define EFI_ACPI_MEMORY_NVS      10

#define EFI_PAGE_SIZE 4096

struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

struct efi_memory_descriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct efi_boot_services {
    struct efi_table_header hdr;

    void *raise_tpl;
    void *restore_tpl;

    efi_status_t (EFI_ABI *allocate_pages)(uint32_t type, uint32_t memory_type,
                                           uint64_t pages, uint64_t *memory);
    efi_status_t (EFI_ABI *free_pages)(uint64_t memory, uint64_t pages);
    efi_status_t (EFI_ABI *get_memory_map)(uint64_t *map_size,
                                           struct efi_memory_descriptor *map,
                                           uint64_t *map_key,
                                           uint64_t *descriptor_size,
                                           uint32_t *descriptor_version);
    void *allocate_pool;
    void *free_pool;

    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;

    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    efi_status_t (EFI_ABI *handle_protocol)(efi_handle_t handle,
                                            const struct efi_guid *protocol,
                                            void **interface);
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;

    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    efi_status_t (EFI_ABI *exit_boot_services)(efi_handle_t image_handle,
                                               uint64_t map_key);

    void *get_next_monotonic_count;
    efi_status_t (EFI_ABI *stall)(uint64_t microseconds);
    void *set_watchdog_timer;

    void *connect_controller;
    void *disconnect_controller;

    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;

    void *protocols_per_handle;
    efi_status_t (EFI_ABI *locate_handle_buffer)(uint32_t search_type,
                                                 const struct efi_guid *protocol,
                                                 void *search_key,
                                                 uint64_t *no_handles,
                                                 efi_handle_t **buffer);
    efi_status_t (EFI_ABI *locate_protocol)(const struct efi_guid *protocol,
                                            void *registration,
                                            void **interface);
};

struct efi_simple_text_output_protocol {
    void *reset;
    efi_status_t (EFI_ABI *output_string)(
        struct efi_simple_text_output_protocol *self, const uint16_t *string);
};

/* The ACPI root pointer is not lying in low memory on a UEFI machine the way it
 * is on a BIOS one; the firmware passes it in the configuration table, and a
 * loader that does not read it there leaves the kernel with no way to find the
 * tables -- and so no way to turn the machine off. */
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, \
      { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, \
      { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

struct efi_configuration_table {
    struct efi_guid vendor_guid;
    void           *vendor_table;
};

struct efi_system_table {
    struct efi_table_header hdr;
    uint16_t *firmware_vendor;
    uint32_t  firmware_revision;
    uint32_t  pad;
    efi_handle_t console_in_handle;
    void     *con_in;
    efi_handle_t console_out_handle;
    struct efi_simple_text_output_protocol *con_out;
    efi_handle_t standard_error_handle;
    void     *std_err;
    void     *runtime_services;
    struct efi_boot_services *boot_services;
    uint64_t  number_of_table_entries;
    struct efi_configuration_table *configuration_table;
};

/* Where the image was loaded from -- the way to the volume holding the rest of
 * the system. */
#define EFI_LOADED_IMAGE_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, \
      { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

struct efi_loaded_image {
    uint32_t     revision;
    uint32_t     pad;
    efi_handle_t parent_handle;
    void        *system_table;
    efi_handle_t device_handle;
};

#define EFI_SIMPLE_FILE_SYSTEM_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_MODE_READ 0x0000000000000001UL

struct efi_file_protocol {
    uint64_t revision;
    efi_status_t (EFI_ABI *open)(struct efi_file_protocol *self,
                                 struct efi_file_protocol **new_handle,
                                 const uint16_t *name,
                                 uint64_t open_mode, uint64_t attributes);
    efi_status_t (EFI_ABI *close)(struct efi_file_protocol *self);
    void *delete;
    efi_status_t (EFI_ABI *read)(struct efi_file_protocol *self,
                                 uint64_t *buffer_size, void *buffer);
    void *write;
    void *get_position;
    void *set_position;
    efi_status_t (EFI_ABI *get_info)(struct efi_file_protocol *self,
                                     const struct efi_guid *information_type,
                                     uint64_t *buffer_size, void *buffer);
};

struct efi_simple_file_system_protocol {
    uint64_t revision;
    efi_status_t (EFI_ABI *open_volume)(
        struct efi_simple_file_system_protocol *self,
        struct efi_file_protocol **root);
};

/* Only the head of EFI_FILE_INFO; the name that follows is not needed. */
struct efi_file_info {
    uint64_t size;
    uint64_t file_size;
    uint64_t physical_size;
    uint8_t  times[48];
    uint64_t attribute;
};

/* The display.  Under UEFI this is the only way to find the framebuffer:
 * there is no VGA text mode to fall back to. */
#define EFI_GOP_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, \
      { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

/* Marks the handle the firmware is actually drawing its console on.  A machine
 * with two graphics devices has a graphics output protocol on each, and only
 * one of them is connected to the screen the user is looking at. */
#define EFI_CONSOLE_OUT_DEVICE_GUID \
    { 0xd3b36f2c, 0xd551, 0x11d4, \
      { 0x9a, 0x46, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

#define EFI_PIXEL_RGB_RESERVED 0
#define EFI_PIXEL_BGR_RESERVED 1
#define EFI_PIXEL_BIT_MASK     2
#define EFI_PIXEL_BLT_ONLY     3

struct efi_gop_mode_info {
    uint32_t version;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixel_format;
    uint32_t pixel_information[4];
    uint32_t pixels_per_scan_line;
};

struct efi_gop_mode {
    uint32_t max_mode;
    uint32_t mode;
    struct efi_gop_mode_info *info;
    uint64_t size_of_info;
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
};

struct efi_graphics_output_protocol {
    void *query_mode;
    void *set_mode;
    void *blt;
    struct efi_gop_mode *mode;
};

#endif /* WOS_UEFI_EFI_H */
