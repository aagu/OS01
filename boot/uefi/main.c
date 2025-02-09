#include <uefi.h>

struct GRAPHICS_INFO
{
	unsigned int HorizontalResolution;
	unsigned int VerticalResolution;
	unsigned int PixelsPerScanLine;

	unsigned long FrameBufferBase;
	unsigned long FrameBufferSize;
};

struct E820_ENTRY
{
	unsigned long address;
	unsigned long length;
	unsigned int  type;
}__attribute__((packed));

struct MEMORY_INFO
{
	unsigned int E820_Entry_count;
	struct E820_ENTRY *E820_Entry;
};

struct BOOT_INFO
{
	struct GRAPHICS_INFO Graphics_Info;
	struct MEMORY_INFO E820_Info;
    unsigned long RSDP;
    boolean_t BootFromBIOS;
};

int EXPECT_VBE_HEIGHT = 900;
int EXPECT_VBE_WIDTH = 1440;

const char *types[] = {
    "EfiReservedMemoryType",
    "EfiLoaderCode",
    "EfiLoaderData",
    "EfiBootServicesCode",
    "EfiBootServicesData",
    "EfiRuntimeServicesCode",
    "EfiRuntimeServicesData",
    "EfiConventionalMemory",
    "EfiUnusableMemory",
    "EfiACPIReclaimMemory",
    "EfiACPIMemoryNVS",
    "EfiMemoryMappedIO",
    "EfiMemoryMappedIOPortSpace",
    "EfiPalCode"
};

struct GRAPHICS_INFO* GetResolution(char *wah) {
    const char delim[2] = "x";
    char_t *w = strtok(wah, delim);
    char_t *h = strtok(NULL, delim);
    struct GRAPHICS_INFO *info = malloc(sizeof(struct GRAPHICS_INFO));
    if (info == NULL)
    {
        return NULL;
    }
    info->VerticalResolution = atoi(h);
    info->HorizontalResolution = atoi(w);
    return info;
}

boolean_t CompareGuid(efi_guid_t * p1, efi_guid_t * p2)
{
    if(p1 == NULL || p2 == NULL)
        return 0;
    if(p1->Data1 != p2->Data1)
        return 0;
    if(p1->Data2 != p2->Data2)
        return 0;
    if(p1->Data3 != p2->Data3)
        return 0;
    return strncmp((char *)p1->Data4, (char *)p2->Data4, 8) == 0;
}

int main(int argc, char **argv)
{
    efi_status_t status;

    // read kernel into memory
    FILE *kernFile = NULL;
    long int kernSize = 0;
    efi_physical_address_t kernel_address = 0x100000;
    if((kernFile = fopen("kernel.bin", "r")))
    {
        // get file size of kernel.bin
        fseek(kernFile, 0, SEEK_END);
        kernSize = ftell(kernFile);
        fseek(kernFile, 0, SEEK_SET);
        // printf("Kernel.bin size: %d bytes\n", kernSize);

        // alloc memory start at 0x100000 for kernel, we use AllocatePages from BootService here because wo want the
        // kernel be load into a fixed address
        status = gBS->AllocatePages(AllocateAddress,EfiLoaderData,(kernSize + 0x1000 - 1) / 0x1000, &kernel_address);
        if(EFI_ERROR(status))
        {
            printf("unable to alloc memory\n");
            return status;
        }

        // read kernel.bin into 0x100000
        // printf("read kernel to memory address:%018lx\n", kernel_address);
        fread((char *)kernel_address, kernSize, 1, kernFile);
        fclose(kernFile);
    }
    else
    {
        fprintf(stderr, "unable to open kernel\n");
        return 1;
    }
    FILE *kconfig = NULL;
    if((kconfig = fopen("config.txt", "r"))) {
        fseek(kconfig, 0, SEEK_END);
        long int size=ftell(kconfig);
        fseek(kconfig, 0, SEEK_SET);
        printf("config.txt found, size %d\n", size);
        char *content = malloc(size + 1);
        if(!content) {
            fprintf(stderr, "unable to allocate memory for config.txt content");
            return 1;
        }
        fread(content, size, 1, kconfig);
        content[size] = 0;
        fclose(kconfig);
        const char delim[2] = " ";
        for (long int i = 0; i <= size;)
        {
            long int j = i;
            for (; j <= size; j++)
            {
                if (content[j] == '\n' || content[j] == 0)
                {
                    long int line_len = j - i;
                    if (line_len == 0){ break; }
                    printf("line length %d\n", line_len);
                    char *line = malloc(line_len + 1);
                    memset(line, 0, line_len);
                    memcpy(line, content+i, line_len);
                    char *tok = strtok(line, delim);
                    if (strcmp(tok, "resolution") == 0)
                    {
                        tok = strtok(NULL, delim);
                        struct GRAPHICS_INFO *info = GetResolution(tok);
                        if (info != NULL)
                        {
                            printf("resolution config: height=%d, width=%d\n", info->VerticalResolution, info->HorizontalResolution);
                            EXPECT_VBE_HEIGHT = info->VerticalResolution;
                            EXPECT_VBE_WIDTH = info->HorizontalResolution;
                            free(info);
                        }
                    }
                    else
                    {
                        printf("unknown config line: %s\n", line);
                    }
                    free(line);
                    break;
                }
            }
            i = j + 1;
        }
        
        free(content);
    }
    else
    {
        printf("config.txt not found, using default config\n");
    }    

    // detect video modes
    efi_guid_t gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    efi_gop_t *gop = NULL;
    efi_gop_mode_info_t *info = NULL;
    uintn_t isiz = sizeof(efi_gop_mode_info_t), i;
    status = BS->LocateProtocol(&gopGuid, NULL, (void**)&gop);
    long currentVBEHeight = 0, currentVBEWidth = 0;
    int expectVBEMode = 0;
    if(!EFI_ERROR(status) && gop) {
        /* iterate on modes and print info */
        for(i = 0; i < gop->Mode->MaxMode; i++) {
            status = gop->QueryMode(gop, i, &isiz, &info);
            if(EFI_ERROR(status) || info->PixelFormat > PixelBitMask) continue;
            printf("VBE mode %d found, width=%d, height=%d\n", i, info->HorizontalResolution, info->VerticalResolution);
            if(info->HorizontalResolution > currentVBEWidth && info->HorizontalResolution <= EXPECT_VBE_WIDTH)
            {
                if(info->VerticalResolution > currentVBEHeight && info->VerticalResolution <= EXPECT_VBE_HEIGHT)
                {
                    currentVBEHeight = info->VerticalResolution;
                    currentVBEWidth = info->HorizontalResolution;
                    expectVBEMode = i;
                }
            }
        }
        printf("set VBE mode to %d\n", expectVBEMode);
        status = gop->SetMode(gop, expectVBEMode);
        if(EFI_ERROR(status)) {
            printf("unable to set video mode\n");
            return 0;
        }
        /* we got the interface, get current mode */
        status = gop->QueryMode(gop, gop->Mode ? gop->Mode->Mode : 0, &isiz, &info);
        if(status == EFI_NOT_STARTED || !gop->Mode) {
            status = gop->SetMode(gop, 0);
            ST->ConOut->Reset(ST->ConOut, 0);
            ST->StdErr->Reset(ST->StdErr, 0);
        }
        if(EFI_ERROR(status)) {
            printf("unable to get current video mode\n");
            return 0;
        }
        printf("current VBE mode:%d,version:%x,horizontal:%d,vertical:%d,framebuffer base:%018lx,framebuffer size:%018lx\n",
            gop->Mode->Mode,
            gop->Mode->Information->Version,
            gop->Mode->Information->HorizontalResolution,
            gop->Mode->Information->VerticalResolution,
            gop->Mode->FrameBufferBase,
            gop->Mode->FrameBufferSize
        );
    } else {
        fprintf(stderr, "unable to get graphics output protocol\n");
        return 1;
    }
    
    efi_physical_address_t boot_param_address = 0x60000;
    status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, 2, &boot_param_address);
    if(EFI_ERROR(status))
    {
        fprintf(stderr, "unable to alloc memory\n");
        return status;
    }
    memset((void *)boot_param_address, 0x1000, 0);
    struct BOOT_INFO * kern_boot_para_info = (struct BOOT_INFO *)boot_param_address;
    kern_boot_para_info->RSDP = 0x0;
    kern_boot_para_info->BootFromBIOS = 0; // may support boot from BIOS later :)
    kern_boot_para_info->Graphics_Info.HorizontalResolution = gop->Mode->Information->HorizontalResolution;
    kern_boot_para_info->Graphics_Info.VerticalResolution = gop->Mode->Information->VerticalResolution;
    kern_boot_para_info->Graphics_Info.PixelsPerScanLine = gop->Mode->Information->PixelsPerScanLine;
    kern_boot_para_info->Graphics_Info.FrameBufferBase = gop->Mode->FrameBufferBase;
    kern_boot_para_info->Graphics_Info.FrameBufferSize = gop->Mode->FrameBufferSize;

    // get memory map
    efi_memory_descriptor_t *memory_map = NULL, *mement;
    uintn_t memory_map_size=0, map_key=0, desc_size=0;

    status = BS->GetMemoryMap(&memory_map_size, NULL, &map_key, &desc_size, NULL);
    if(status != EFI_BUFFER_TOO_SMALL || !memory_map_size) goto err;
    /* in worst case malloc allocates two blocks, and each block might split a record into three, that's 4 additional records */
    memory_map_size += 4 * desc_size;
    memory_map = (efi_memory_descriptor_t*)malloc(memory_map_size);
    if(!memory_map) {
        fprintf(stderr, "unable to allocate memory\n");
        return 1;
    }
    status = BS->GetMemoryMap(&memory_map_size, memory_map, &map_key, &desc_size, NULL);
    if(EFI_ERROR(status)) {
err:    fprintf(stderr, "Unable to get memory map\n");
        return 1;
    }

	struct E820_ENTRY *LastE820 = NULL;
    struct E820_ENTRY *E820p = kern_boot_para_info->E820_Info.E820_Entry;
	unsigned long LastEndAddr = 0;
	int E820Count = 0;

    printf("Address              Size Type\n");
    for(mement = memory_map; (uint8_t*)mement < (uint8_t*)memory_map + memory_map_size; mement = NextMemoryDescriptor(mement, desc_size)) {
        int MemType = 0;
        #ifdef DEBUG
        printf("%016x %8d %02x %s\n", mement->PhysicalStart, mement->NumberOfPages, mement->Type, types[mement->Type]);
        #endif
        switch (mement->Type)
        {
        case EfiReservedMemoryType:
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace:
        case EfiPalCode:
            MemType = 2;    //2:ROM or Reserved
            break;
            
        case EfiUnusableMemory:
            MemType = 5;    //5:Unusable
            break;

        case EfiACPIReclaimMemory:
            MemType = 3;    //3:ACPI Reclaim Memory
            break;

        case EfiLoaderCode:
		case EfiLoaderData:
		case EfiBootServicesCode:
		case EfiBootServicesData:
		case EfiRuntimeServicesCode:
		case EfiRuntimeServicesData:
		case EfiConventionalMemory:
			MemType = 1;	//1:RAM
			break;
            
        case EfiACPIMemoryNVS:
			MemType = 4;	//4:ACPI NVS Memory
			break;

		default:
			printf("Invalid UEFI Memory Type:%4d\n",mement->Type);
			continue;

        }
        if((LastE820 != NULL) && (LastE820->type == MemType) && (mement->PhysicalStart == LastEndAddr))
	    {
	    	LastE820->length += mement->NumberOfPages << 12;
	    	LastEndAddr += mement->NumberOfPages << 12;
	    }
	    else
	    {
	    	E820p->address = mement->PhysicalStart;
	    	E820p->length = mement->NumberOfPages << 12;
	    	E820p->type = MemType;
	    	LastEndAddr = mement->PhysicalStart + (mement->NumberOfPages << 12);
	    	LastE820 = E820p;
	    	E820p++;
	    	E820Count++;			
	    }
    }

    free(memory_map);

    kern_boot_para_info->E820_Info.E820_Entry_count = E820Count;
    LastE820 = kern_boot_para_info->E820_Info.E820_Entry;
    printf("E820 count %d\n", E820Count);
    int j = 0;
	for(i = 0; i< E820Count; i++)
	{
		struct E820_ENTRY* e820i = LastE820 + i;
		struct E820_ENTRY MemMap;
		for(j = i + 1; j< E820Count; j++)
		{
			struct E820_ENTRY* e820j = LastE820 + j;
			if(e820i->address > e820j->address)
			{
				MemMap = *e820i;
				*e820i = *e820j;
				*e820j = MemMap;
			}
		}
	}

	LastE820 = kern_boot_para_info->E820_Info.E820_Entry;
	for(i = 0;i < E820Count;i++)
	{
		printf("MemoryMap (%10lx<->%10lx) %4d\n",LastE820->address,LastE820->address+LastE820->length,LastE820->type);
		LastE820++;
	}

    // find RSDP
    efi_configuration_table_t* configTable = ST->ConfigurationTable;
	efi_guid_t Acpi2TableGuid = ACPI_20_TABLE_GUID;

	for (uintn_t index = 0; index < ST->NumberOfTableEntries; index++)
	{
		if (CompareGuid(&configTable[index].VendorGuid, &Acpi2TableGuid))
		{
            printf("Acpi2TableGuid found, index %d, address %018lx\n", index, configTable[index].VendorTable);
            kern_boot_para_info->RSDP = (unsigned long)configTable[index].VendorTable;
            break;
		}
		configTable++;
	}

    if(kern_boot_para_info->RSDP == 0)
        printf("RSDP not found!\n");
    // exit BootService and jump to kernel
    if(exit_bs()) {
        fprintf(stderr, "error when exit boot service!\n");
        return 0;
    }
    int (*kernel_main)(struct BOOT_INFO *);
    kernel_main = (void*)0x100000;

    kernel_main(kern_boot_para_info);
    // should never get here
    while(1);
    return EFI_SUCCESS;
}