/*
 * PROJECT:         EFI Windows Loader
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            boot/freeldr/freeldr/windows/wlregistry.c
 * PURPOSE:         Registry support functions
 * PROGRAMMERS:     Aleksey Bragin (aleksey@reactos.org)
 */

/* INCLUDES ***************************************************************/

#include <freeldr.h>
#include "winldr.h"
#include "registry.h"

#include <debug.h>
DBG_DEFAULT_CHANNEL(WINDOWS);

// The only global var here, used to mark mem pages as NLS in WinLdrSetupMemoryLayout()
ULONG TotalNLSSize = 0;

static BOOLEAN
WinLdrGetNLSNames(
    _Out_z_bytecap_(AnsiNameSize) PSTR AnsiName,
    _In_ SIZE_T AnsiNameSize,
    _Out_z_bytecap_(OemNameSize)  PSTR OemName,
    _In_ SIZE_T OemNameSize,
    _Out_z_bytecap_(LangNameSize) PSTR LangName,
    _In_ SIZE_T LangNameSize);

static VOID
WinLdrScanRegistry(IN OUT PLIST_ENTRY BootDriverListHead,
                   IN PCSTR SystemRoot);


/* FUNCTIONS **************************************************************/

static BOOLEAN
WinLdrLoadSystemHive(
    IN OUT PLOADER_PARAMETER_BLOCK LoaderBlock,
    IN PCSTR DirectoryPath,
    IN PCSTR HiveName)
{
    ULONG FileId;
    CHAR FullHiveName[MAX_PATH];
    ARC_STATUS Status;
    FILEINFORMATION FileInfo;
    ULONG HiveFileSize;
    PVOID HiveDataPhysical;
    PVOID HiveDataVirtual;
    ULONG BytesRead;
    PCWSTR FsService;

    /* Concatenate path and filename to get the full name */
    RtlStringCbCopyA(FullHiveName, sizeof(FullHiveName), DirectoryPath);
    RtlStringCbCatA(FullHiveName, sizeof(FullHiveName), HiveName);

    NtLdrOutputLoadMsg(FullHiveName, NULL);
    Status = ArcOpen(FullHiveName, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        WARN("Error while opening '%s', Status: %u\n", FullHiveName, Status);
        return FALSE;
    }

    /* Get the file length */
    Status = ArcGetFileInformation(FileId, &FileInfo);
    if (Status != ESUCCESS)
    {
        WARN("Hive file has 0 size!");
        ArcClose(FileId);
        return FALSE;
    }
    HiveFileSize = FileInfo.EndingAddress.LowPart;

    /* Round up the size to page boundary and alloc memory */
    HiveDataPhysical = MmAllocateMemoryWithType(
        MM_SIZE_TO_PAGES(HiveFileSize + MM_PAGE_SIZE - 1) << MM_PAGE_SHIFT,
        LoaderRegistryData);

    if (HiveDataPhysical == NULL)
    {
        WARN("Could not alloc memory for hive!");
        ArcClose(FileId);
        return FALSE;
    }

    /* Convert address to virtual */
    HiveDataVirtual = PaToVa(HiveDataPhysical);

    /* Fill LoaderBlock's entries */
    LoaderBlock->RegistryLength = HiveFileSize;
    LoaderBlock->RegistryBase = HiveDataVirtual;

    /* Finally read from file to the memory */
    Status = ArcRead(FileId, HiveDataPhysical, HiveFileSize, &BytesRead);
    if (Status != ESUCCESS)
    {
        WARN("Error while reading '%s', Status: %u\n", FullHiveName, Status);
        ArcClose(FileId);
        return FALSE;
    }

    /* Add boot filesystem driver to the list */
    FsService = FsGetServiceName(FileId);
    if (FsService)
    {
        BOOLEAN Success;
        TRACE("Adding filesystem service %S\n", FsService);
        Success = WinLdrAddDriverToList(&LoaderBlock->BootDriverListHead,
                                        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\",
                                        NULL,
                                        (PWSTR)FsService);
        if (!Success)
            TRACE("Failed to add filesystem service\n");
    }
    else
    {
        TRACE("No required filesystem service\n");
    }

    ArcClose(FileId);
    return TRUE;
}

BOOLEAN
WinLdrInitSystemHive(
    IN OUT PLOADER_PARAMETER_BLOCK LoaderBlock,
    IN PCSTR SystemRoot,
    IN BOOLEAN Setup)
{
    CHAR SearchPath[1024];
    PCSTR HiveName;
    BOOLEAN Success;

    if (Setup)
    {
        RtlStringCbCopyA(SearchPath, sizeof(SearchPath), SystemRoot);
        HiveName = "SETUPREG.HIV";
    }
    else
    {
        // There is a simple logic here: try to load usual hive (system), if it
        // fails, then give system.alt a try, and finally try a system.sav

        // FIXME: For now we only try system
        RtlStringCbCopyA(SearchPath, sizeof(SearchPath), SystemRoot);
        RtlStringCbCatA(SearchPath, sizeof(SearchPath), "system32\\config\\");
        HiveName = "SYSTEM";
    }

    TRACE("WinLdrInitSystemHive: loading hive %s%s\n", SearchPath, HiveName);
    Success = WinLdrLoadSystemHive(LoaderBlock, SearchPath, HiveName);
    if (!Success)
    {
        UiMessageBox("Could not load %s hive!", HiveName);
        return FALSE;
    }

    /* Import what was loaded */
    Success = RegImportBinaryHive(VaToPa(LoaderBlock->RegistryBase), LoaderBlock->RegistryLength);
    if (!Success)
    {
        UiMessageBox("Importing binary hive failed!");
        return FALSE;
    }

    /* Initialize the 'CurrentControlSet' link */
    if (RegInitCurrentControlSet(FALSE) != ERROR_SUCCESS)
    {
        UiMessageBox("Initializing CurrentControlSet link failed!");
        return FALSE;
    }

    return TRUE;
}

BOOLEAN WinLdrScanSystemHive(IN OUT PLOADER_PARAMETER_BLOCK LoaderBlock,
                             IN PCSTR SystemRoot)
{
    BOOLEAN Success;
    CHAR SearchPath[1024];
    CHAR AnsiName[256], OemName[256], LangName[256];

    /* Scan registry and prepare boot drivers list */
    WinLdrScanRegistry(&LoaderBlock->BootDriverListHead, SystemRoot);

    /* Get names of NLS files */
    Success = WinLdrGetNLSNames(AnsiName, sizeof(AnsiName),
                                OemName, sizeof(OemName),
                                LangName, sizeof(LangName));
    if (!Success)
    {
        UiMessageBox("Getting NLS names from registry failed!");
        return FALSE;
    }

    TRACE("NLS data %s %s %s\n", AnsiName, OemName, LangName);

    /* Load NLS data */
    RtlStringCbCopyA(SearchPath, sizeof(SearchPath), SystemRoot);
    RtlStringCbCatA(SearchPath, sizeof(SearchPath), "system32\\");
    Success = WinLdrLoadNLSData(LoaderBlock, SearchPath, AnsiName, OemName, LangName);
    TRACE("NLS data loading %s\n", Success ? "successful" : "failed");

    /* TODO: Load OEM HAL font */
    // In HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Nls\CodePage,
    // REG_SZ value "OEMHAL"

    return TRUE;
}


/* PRIVATE FUNCTIONS ******************************************************/

// Queries registry for those three file names
_Use_decl_annotations_
static BOOLEAN
WinLdrGetNLSNames(
    _Out_ PSTR AnsiName,
    _In_ SIZE_T AnsiNameSize,
    _Out_ PSTR OemName,
    _In_ SIZE_T OemNameSize,
    _Out_ PSTR LangName,
    _In_ SIZE_T LangNameSize)
{
    LONG rc = ERROR_SUCCESS;
    HKEY hKey;
    ULONG BufferSize;
    WCHAR szIdBuffer[80];
    WCHAR NameBuffer[80];

    /* Open the CodePage key */
    rc = RegOpenKey(NULL,
                    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\NLS\\CodePage",
                    &hKey);
    if (rc != ERROR_SUCCESS)
    {
        //TRACE("Couldn't open CodePage registry key");
        return FALSE;
    }

    /* Get ANSI codepage */
    BufferSize = sizeof(szIdBuffer);
    rc = RegQueryValue(hKey, L"ACP", NULL, (PUCHAR)szIdBuffer, &BufferSize);
    if (rc != ERROR_SUCCESS)
    {
        //TRACE("Couldn't get ACP NLS setting");
        RegCloseKey(hKey);
        return FALSE;
    }

    BufferSize = sizeof(NameBuffer);
    rc = RegQueryValue(hKey, szIdBuffer, NULL, (PUCHAR)NameBuffer, &BufferSize);
    if (rc != ERROR_SUCCESS)
    {
        //TRACE("ACP NLS Setting exists, but isn't readable");
        //RegCloseKey(hKey);
        //return FALSE;
        wcscpy(NameBuffer, L"c_1252.nls"); // HACK: ReactOS bug CORE-6105
    }
    RtlStringCbPrintfA(AnsiName, AnsiNameSize, "%S", NameBuffer);

    /* Get OEM codepage */
    BufferSize = sizeof(szIdBuffer);
    rc = RegQueryValue(hKey, L"OEMCP", NULL, (PUCHAR)szIdBuffer, &BufferSize);
    if (rc != ERROR_SUCCESS)
    {
        //TRACE("Couldn't get OEMCP NLS setting");
        RegCloseKey(hKey);
        return FALSE;
    }

    BufferSize = sizeof(NameBuffer);
    rc = RegQueryValue(hKey, szIdBuffer, NULL, (PUCHAR)NameBuffer, &BufferSize);
    if (rc != ERROR_SUCCESS)
    {
        //TRACE("OEMCP NLS setting exists, but isn't readable");
        //RegCloseKey(hKey);
        //return FALSE;
        wcscpy(NameBuffer, L"c_437.nls"); // HACK: ReactOS bug CORE-6105
    }
    RtlStringCbPrintfA(OemName, OemNameSize, "%S", NameBuffer);

    RegCloseKey(hKey);

    /* Open the Language key */
    rc = RegOpenKey(NULL,
                    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\NLS\\Language",
                    &hKey);
    if (rc != ERROR_SUCCESS)
    {
        //TRACE("Couldn't open Language registry key");
        return FALSE;
    }

    /* Get the Unicode case table */
    BufferSize = sizeof(szIdBuffer);
    rc = RegQueryValue(hKey, L"Default", NULL, (PUCHAR)szIdBuffer, &BufferSize);
    if (rc != ERROR_SUCCESS)
    {
        //TRACE("Couldn't get Language Default setting");
        RegCloseKey(hKey);
        return FALSE;
    }

    BufferSize = sizeof(NameBuffer);
    rc = RegQueryValue(hKey, szIdBuffer, NULL, (PUCHAR)NameBuffer, &BufferSize);
    if (rc != ERROR_SUCCESS)
    {
        //TRACE("Language Default setting exists, but isn't readable");
        RegCloseKey(hKey);
        return FALSE;
    }
    RtlStringCbPrintfA(LangName, LangNameSize, "%S", NameBuffer);

    RegCloseKey(hKey);
    return TRUE;
}

BOOLEAN
WinLdrLoadNLSData(IN OUT PLOADER_PARAMETER_BLOCK LoaderBlock,
                  IN PCSTR DirectoryPath,
                  IN PCSTR AnsiFileName,
                  IN PCSTR OemFileName,
                  IN PCSTR LanguageFileName)
{
    CHAR FileName[255];
    ULONG FileId;
    ULONG AnsiFileSize, OemFileSize, LanguageFileSize;
    ULONG TotalSize;
    PVOID NlsDataBase;
    PVOID NlsVirtual;
    BOOLEAN AnsiEqualsOem = FALSE;
    FILEINFORMATION FileInfo;
    ULONG BytesRead;
    ARC_STATUS Status;

    /* There may be a case, when OEM and ANSI page coincide */
    if (!strcmp(AnsiFileName, OemFileName))
        AnsiEqualsOem = TRUE;

    /* Open file with ANSI and store its size */
    RtlStringCbCopyA(FileName, sizeof(FileName), DirectoryPath);
    RtlStringCbCatA(FileName, sizeof(FileName), AnsiFileName);

    NtLdrOutputLoadMsg(FileName, NULL);
    Status = ArcOpen(FileName, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        WARN("Error while opening '%s', Status: %u\n", FileName, Status);
        goto Failure;
    }

    Status = ArcGetFileInformation(FileId, &FileInfo);
    ArcClose(FileId);
    if (Status != ESUCCESS)
        goto Failure;
    AnsiFileSize = FileInfo.EndingAddress.LowPart;
    TRACE("AnsiFileSize: %d\n", AnsiFileSize);

    /* Open OEM file and store its length */
    if (AnsiEqualsOem)
    {
        OemFileSize = 0;
    }
    else
    {
        RtlStringCbCopyA(FileName, sizeof(FileName), DirectoryPath);
        RtlStringCbCatA(FileName, sizeof(FileName), OemFileName);

        NtLdrOutputLoadMsg(FileName, NULL);
        Status = ArcOpen(FileName, OpenReadOnly, &FileId);
        if (Status != ESUCCESS)
        {
            WARN("Error while opening '%s', Status: %u\n", FileName, Status);
            goto Failure;
        }

        Status = ArcGetFileInformation(FileId, &FileInfo);
        ArcClose(FileId);
        if (Status != ESUCCESS)
            goto Failure;
        OemFileSize = FileInfo.EndingAddress.LowPart;
    }
    TRACE("OemFileSize: %d\n", OemFileSize);

    /* And finally open the language codepage file and store its length */
    RtlStringCbCopyA(FileName, sizeof(FileName), DirectoryPath);
    RtlStringCbCatA(FileName, sizeof(FileName), LanguageFileName);

    NtLdrOutputLoadMsg(FileName, NULL);
    Status = ArcOpen(FileName, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        WARN("Error while opening '%s', Status: %u\n", FileName, Status);
        goto Failure;
    }

    Status = ArcGetFileInformation(FileId, &FileInfo);
    ArcClose(FileId);
    if (Status != ESUCCESS)
        goto Failure;
    LanguageFileSize = FileInfo.EndingAddress.LowPart;
    TRACE("LanguageFileSize: %d\n", LanguageFileSize);

    /* Sum up all three length, having in mind that every one of them
       must start at a page boundary => thus round up each file to a page */
    TotalSize = MM_SIZE_TO_PAGES(AnsiFileSize) +
                MM_SIZE_TO_PAGES(OemFileSize)  +
                MM_SIZE_TO_PAGES(LanguageFileSize);

    /* Store it for later marking the pages as NlsData type */
    TotalNLSSize = TotalSize;

    NlsDataBase = MmAllocateMemoryWithType(TotalSize*MM_PAGE_SIZE, LoaderNlsData);
    if (NlsDataBase == NULL)
        goto Failure;

    NlsVirtual = PaToVa(NlsDataBase);
    LoaderBlock->NlsData->AnsiCodePageData = NlsVirtual;
    LoaderBlock->NlsData->OemCodePageData = (PVOID)((ULONG_PTR)NlsVirtual +
        (MM_SIZE_TO_PAGES(AnsiFileSize) << MM_PAGE_SHIFT));
    LoaderBlock->NlsData->UnicodeCodePageData = (PVOID)((ULONG_PTR)NlsVirtual +
        (MM_SIZE_TO_PAGES(AnsiFileSize) << MM_PAGE_SHIFT) +
        (MM_SIZE_TO_PAGES(OemFileSize)  << MM_PAGE_SHIFT));

    /* Ansi and OEM data are the same - just set pointers to the same area */
    if (AnsiEqualsOem)
        LoaderBlock->NlsData->OemCodePageData = LoaderBlock->NlsData->AnsiCodePageData;

    /* Now actually read the data into memory, starting with Ansi file */
    RtlStringCbCopyA(FileName, sizeof(FileName), DirectoryPath);
    RtlStringCbCatA(FileName, sizeof(FileName), AnsiFileName);

    NtLdrOutputLoadMsg(FileName, NULL);
    Status = ArcOpen(FileName, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        WARN("Error while opening '%s', Status: %u\n", FileName, Status);
        goto Failure;
    }

    Status = ArcRead(FileId, VaToPa(LoaderBlock->NlsData->AnsiCodePageData), AnsiFileSize, &BytesRead);
    ArcClose(FileId);
    if (Status != ESUCCESS)
    {
        WARN("Error while reading '%s', Status: %u\n", FileName, Status);
        goto Failure;
    }

    /* OEM now, if it doesn't equal Ansi of course */
    if (!AnsiEqualsOem)
    {
        RtlStringCbCopyA(FileName, sizeof(FileName), DirectoryPath);
        RtlStringCbCatA(FileName, sizeof(FileName), OemFileName);

        NtLdrOutputLoadMsg(FileName, NULL);
        Status = ArcOpen(FileName, OpenReadOnly, &FileId);
        if (Status != ESUCCESS)
        {
            WARN("Error while opening '%s', Status: %u\n", FileName, Status);
            goto Failure;
        }

        Status = ArcRead(FileId, VaToPa(LoaderBlock->NlsData->OemCodePageData), OemFileSize, &BytesRead);
        ArcClose(FileId);
        if (Status != ESUCCESS)
        {
            WARN("Error while reading '%s', Status: %u\n", FileName, Status);
            goto Failure;
        }
    }

    /* Finally the language file */
    RtlStringCbCopyA(FileName, sizeof(FileName), DirectoryPath);
    RtlStringCbCatA(FileName, sizeof(FileName), LanguageFileName);

    NtLdrOutputLoadMsg(FileName, NULL);
    Status = ArcOpen(FileName, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        WARN("Error while opening '%s', Status: %u\n", FileName, Status);
        goto Failure;
    }

    Status = ArcRead(FileId, VaToPa(LoaderBlock->NlsData->UnicodeCodePageData), LanguageFileSize, &BytesRead);
    ArcClose(FileId);
    if (Status != ESUCCESS)
    {
        WARN("Error while reading '%s', Status: %u\n", FileName, Status);
        goto Failure;
    }

    //
    // THIS IS HAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACK
    // Should go to WinLdrLoadOemHalFont(), when it will be implemented
    //
    LoaderBlock->OemFontFile = VaToPa(LoaderBlock->NlsData->UnicodeCodePageData);

    /* Convert NlsTables address to VA */
    LoaderBlock->NlsData = PaToVa(LoaderBlock->NlsData);

    return TRUE;

Failure:
    UiMessageBox("Error reading NLS file %s", FileName);
    return FALSE;
}

static VOID
WinLdrScanRegistry(IN OUT PLIST_ENTRY BootDriverListHead,
                   IN PCSTR SystemRoot)
{
    LONG rc = 0;
    HKEY hOrderKey, hServiceKey, hGroupKey, hDriverKey;
    PWSTR GroupNameBuffer = NULL;
    WCHAR ServiceName[256];
    ULONG OrderList[128];
    ULONG BufferSize;
    ULONG Index;
    ULONG TagIndex;
    PWSTR GroupName;

    ULONG ValueSize;
    ULONG ValueType;
    ULONG StartValue;
    ULONG TagValue;
    WCHAR DriverGroup[256];
    ULONG DriverGroupSize;

    CHAR ImagePath[256];
    WCHAR TempImagePath[256];

    BOOLEAN Success;

    /* Get 'group order list' key */
    rc = RegOpenKey(NULL,
                    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\GroupOrderList",
                    &hOrderKey);
    if (rc != ERROR_SUCCESS)
    {
        TRACE_CH(REACTOS, "Failed to open the 'GroupOrderList' key (rc %d)\n", (int)rc);
        return;
    }

    /* Get 'services' key */
    rc = RegOpenKey(NULL,
                    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services",
                    &hServiceKey);
    if (rc != ERROR_SUCCESS)
    {
        TRACE_CH(REACTOS, "Failed to open the 'Services' key (rc %d)\n", (int)rc);
        RegCloseKey(hOrderKey);
        return;
    }

    /* Get 'service group order' key */
    rc = RegOpenKey(NULL,
                    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\ServiceGroupOrder",
                    &hGroupKey);
    if (rc != ERROR_SUCCESS)
    {
        TRACE_CH(REACTOS, "Failed to open the 'ServiceGroupOrder' key (rc %d)\n", (int)rc);
        goto Quit;
    }

    /* Get the Group Order List */
    BufferSize = 4096;
    GroupNameBuffer = FrLdrHeapAlloc(BufferSize, TAG_WLDR_NAME);
    if (!GroupNameBuffer)
    {
        TRACE_CH(REACTOS, "Failed to allocate buffer\n");
        RegCloseKey(hGroupKey);
        goto Quit;
    }
    rc = RegQueryValue(hGroupKey, L"List", NULL, (PUCHAR)GroupNameBuffer, &BufferSize);
    RegCloseKey(hGroupKey);

    if (rc != ERROR_SUCCESS)
    {
        TRACE_CH(REACTOS, "Failed to query the 'List' value (rc %d)\n", (int)rc);
        goto Quit;
    }
    TRACE_CH(REACTOS, "BufferSize: %d\n", (int)BufferSize);
    TRACE_CH(REACTOS, "GroupNameBuffer: '%S'\n", GroupNameBuffer);

    /* Loop through each group */
    GroupName = GroupNameBuffer;
    while (*GroupName)
    {
        TRACE("Driver group: '%S'\n", GroupName);

        /* Query the Order */
        BufferSize = sizeof(OrderList);
        rc = RegQueryValue(hOrderKey, GroupName, NULL, (PUCHAR)OrderList, &BufferSize);
        if (rc != ERROR_SUCCESS) OrderList[0] = 0;

        /* Enumerate all drivers */
        for (TagIndex = 1; TagIndex <= OrderList[0]; TagIndex++)
        {
            for (Index = 0; TRUE; Index++)
            {
                /* Get the Driver's Name */
                ValueSize = sizeof(ServiceName);
                rc = RegEnumKey(hServiceKey, Index, ServiceName, &ValueSize, &hDriverKey);
                TRACE("RegEnumKey(): rc %d\n", (int)rc);

                /* Make sure it's valid, and check if we're done */
                if (rc == ERROR_NO_MORE_ITEMS)
                    break;
                if (rc != ERROR_SUCCESS)
                    goto Quit;
                //TRACE_CH(REACTOS, "Service %d: '%S'\n", (int)Index, ServiceName);

                /* Read the Start Value */
                ValueSize = sizeof(ULONG);
                rc = RegQueryValue(hDriverKey, L"Start", &ValueType, (PUCHAR)&StartValue, &ValueSize);
                if (rc != ERROR_SUCCESS) StartValue = (ULONG)-1;
                //TRACE_CH(REACTOS, "  Start: %x\n", (int)StartValue);

                /* Read the Tag */
                ValueSize = sizeof(ULONG);
                rc = RegQueryValue(hDriverKey, L"Tag", &ValueType, (PUCHAR)&TagValue, &ValueSize);
                if (rc != ERROR_SUCCESS) TagValue = (ULONG)-1;
                //TRACE_CH(REACTOS, "  Tag:   %x\n", (int)TagValue);

                /* Read the driver's group */
                DriverGroupSize = sizeof(DriverGroup);
                rc = RegQueryValue(hDriverKey, L"Group", NULL, (PUCHAR)DriverGroup, &DriverGroupSize);
                //TRACE_CH(REACTOS, "  Group: '%S'\n", DriverGroup);

                /* Make sure it should be started */
                if ((StartValue == 0) &&
                    (TagValue == OrderList[TagIndex]) &&
                    (_wcsicmp(DriverGroup, GroupName) == 0))
                {
                    /* Get the Driver's Location */
                    ValueSize = sizeof(TempImagePath);
                    rc = RegQueryValue(hDriverKey, L"ImagePath", NULL, (PUCHAR)TempImagePath, &ValueSize);

                    /* Write the whole path if it succeeded, else prepare to fail */
                    if (rc != ERROR_SUCCESS)
                    {
                        TRACE_CH(REACTOS, "ImagePath: not found\n");
                        TempImagePath[0] = 0;
                        RtlStringCbPrintfA(ImagePath, sizeof(ImagePath), "%s\\system32\\drivers\\%S.sys", SystemRoot, ServiceName);
                    }
                    else if (TempImagePath[0] != L'\\')
                    {
                        RtlStringCbPrintfA(ImagePath, sizeof(ImagePath), "%s%S", SystemRoot, TempImagePath);
                    }
                    else
                    {
                        RtlStringCbPrintfA(ImagePath, sizeof(ImagePath), "%S", TempImagePath);
                        TRACE_CH(REACTOS, "ImagePath: '%s'\n", ImagePath);
                    }

                    TRACE("Adding boot driver: '%s'\n", ImagePath);

                    Success = WinLdrAddDriverToList(BootDriverListHead,
                                                    L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\",
                                                    TempImagePath,
                                                    ServiceName);
                    if (!Success)
                        ERR("Failed to add boot driver\n");
                }
                else
                {
                    //TRACE("  Skipping driver '%S' with Start %d, Tag %d and Group '%S' (Current Tag %d, current group '%S')\n",
                    //    ServiceName, StartValue, TagValue, DriverGroup, OrderList[TagIndex], GroupName);
                }

                RegCloseKey(hDriverKey);
            }
        }

        for (Index = 0; TRUE; Index++)
        {
            /* Get the Driver's Name */
            ValueSize = sizeof(ServiceName);
            rc = RegEnumKey(hServiceKey, Index, ServiceName, &ValueSize, &hDriverKey);

            //TRACE_CH(REACTOS, "RegEnumKey(): rc %d\n", (int)rc);
            if (rc == ERROR_NO_MORE_ITEMS)
                break;
            if (rc != ERROR_SUCCESS)
                goto Quit;
            TRACE("Service %d: '%S'\n", (int)Index, ServiceName);

            /* Read the Start Value */
            ValueSize = sizeof(ULONG);
            rc = RegQueryValue(hDriverKey, L"Start", &ValueType, (PUCHAR)&StartValue, &ValueSize);
            if (rc != ERROR_SUCCESS) StartValue = (ULONG)-1;
            //TRACE_CH(REACTOS, "  Start: %x\n", (int)StartValue);

            /* Read the Tag */
            ValueSize = sizeof(ULONG);
            rc = RegQueryValue(hDriverKey, L"Tag", &ValueType, (PUCHAR)&TagValue, &ValueSize);
            if (rc != ERROR_SUCCESS) TagValue = (ULONG)-1;
            //TRACE_CH(REACTOS, "  Tag:   %x\n", (int)TagValue);

            /* Read the driver's group */
            DriverGroupSize = sizeof(DriverGroup);
            rc = RegQueryValue(hDriverKey, L"Group", NULL, (PUCHAR)DriverGroup, &DriverGroupSize);
            //TRACE_CH(REACTOS, "  Group: '%S'\n", DriverGroup);

            for (TagIndex = 1; TagIndex <= OrderList[0]; TagIndex++)
            {
                if (TagValue == OrderList[TagIndex]) break;
            }

            if ((StartValue == 0) &&
                (TagIndex > OrderList[0]) &&
                (_wcsicmp(DriverGroup, GroupName) == 0))
            {
                ValueSize = sizeof(TempImagePath);
                rc = RegQueryValue(hDriverKey, L"ImagePath", NULL, (PUCHAR)TempImagePath, &ValueSize);
                if (rc != ERROR_SUCCESS)
                {
                    TRACE_CH(REACTOS, "ImagePath: not found\n");
                    TempImagePath[0] = 0;
                    RtlStringCbPrintfA(ImagePath, sizeof(ImagePath), "%ssystem32\\drivers\\%S.sys", SystemRoot, ServiceName);
                }
                else if (TempImagePath[0] != L'\\')
                {
                    RtlStringCbPrintfA(ImagePath, sizeof(ImagePath), "%s%S", SystemRoot, TempImagePath);
                }
                else
                {
                    RtlStringCbPrintfA(ImagePath, sizeof(ImagePath), "%S", TempImagePath);
                    TRACE_CH(REACTOS, "ImagePath: '%s'\n", ImagePath);
                }
                TRACE("  Adding boot driver: '%s'\n", ImagePath);

                Success = WinLdrAddDriverToList(BootDriverListHead,
                                                L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\",
                                                TempImagePath,
                                                ServiceName);
                if (!Success)
                    ERR(" Failed to add boot driver\n");
            }
            else
            {
                //TRACE("  Skipping driver '%S' with Start %d, Tag %d and Group '%S' (Current group '%S')\n",
                //    ServiceName, StartValue, TagValue, DriverGroup, GroupName);
            }

            RegCloseKey(hDriverKey);
        }

        /* Move to the next group name */
        GroupName = GroupName + wcslen(GroupName) + 1;
    }

Quit:
    /* Free allocated memory */
    if (GroupNameBuffer)
        FrLdrHeapFree(GroupNameBuffer, TAG_WLDR_NAME);

    /* Close the registry key handles */
    RegCloseKey(hServiceKey);
    RegCloseKey(hOrderKey);
}

static
BOOLEAN
InsertInBootDriverList(
    PLIST_ENTRY BootDriverListHead,
    PBOOT_DRIVER_LIST_ENTRY BootDriverEntry)
{
    PBOOT_DRIVER_LIST_ENTRY DriverEntry;
    PLIST_ENTRY ListEntry;

    ASSERT(BootDriverEntry->FilePath.Buffer != NULL);
    ASSERT(BootDriverEntry->RegistryPath.Buffer != NULL);

    for (ListEntry = BootDriverListHead->Flink;
         ListEntry != BootDriverListHead;
         ListEntry = ListEntry->Flink)
    {
        DriverEntry = CONTAINING_RECORD(ListEntry,
                                        BOOT_DRIVER_LIST_ENTRY,
                                        Link);
        if ((DriverEntry->FilePath.Buffer != NULL) &&
            RtlEqualUnicodeString(&BootDriverEntry->FilePath,
                                  &DriverEntry->FilePath,
                                  TRUE))
        {
            return FALSE;
        }

        if ((DriverEntry->RegistryPath.Buffer != NULL) &&
            RtlEqualUnicodeString(&BootDriverEntry->RegistryPath,
                                  &DriverEntry->RegistryPath,
                                  TRUE))
        {
            return FALSE;
        }
    }

    InsertTailList(BootDriverListHead, &BootDriverEntry->Link);
    return TRUE;
}

BOOLEAN
WinLdrAddDriverToList(LIST_ENTRY *BootDriverListHead,
                      PWSTR RegistryPath,
                      PWSTR ImagePath,
                      PWSTR ServiceName)
{
    PBOOT_DRIVER_LIST_ENTRY BootDriverEntry;
    NTSTATUS Status;
    USHORT PathLength;

    BootDriverEntry = FrLdrHeapAlloc(sizeof(BOOT_DRIVER_LIST_ENTRY), TAG_WLDR_BDE);
    if (!BootDriverEntry)
        return FALSE;

    // DTE will be filled during actual load of the driver
    BootDriverEntry->LdrEntry = NULL;

    // Check - if we have a valid ImagePath, if not - we need to build it
    // like "System32\\Drivers\\blah.sys"
    if (ImagePath && (ImagePath[0] != 0))
    {
        // Just copy ImagePath to the corresponding field in the structure
        PathLength = (USHORT)wcslen(ImagePath) * sizeof(WCHAR) + sizeof(UNICODE_NULL);

        BootDriverEntry->FilePath.Length = 0;
        BootDriverEntry->FilePath.MaximumLength = PathLength;
        BootDriverEntry->FilePath.Buffer = FrLdrHeapAlloc(PathLength, TAG_WLDR_NAME);
        if (!BootDriverEntry->FilePath.Buffer)
        {
            FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
            return FALSE;
        }

        Status = RtlAppendUnicodeToString(&BootDriverEntry->FilePath, ImagePath);
        if (!NT_SUCCESS(Status))
        {
            FrLdrHeapFree(BootDriverEntry->FilePath.Buffer, TAG_WLDR_NAME);
            FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
            return FALSE;
        }
    }
    else
    {
        // we have to construct ImagePath ourselves
        PathLength = (USHORT)wcslen(ServiceName)*sizeof(WCHAR) + sizeof(L"system32\\drivers\\.sys");
        BootDriverEntry->FilePath.Length = 0;
        BootDriverEntry->FilePath.MaximumLength = PathLength;
        BootDriverEntry->FilePath.Buffer = FrLdrHeapAlloc(PathLength, TAG_WLDR_NAME);
        if (!BootDriverEntry->FilePath.Buffer)
        {
            FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
            return FALSE;
        }

        Status = RtlAppendUnicodeToString(&BootDriverEntry->FilePath, L"system32\\drivers\\");
        if (!NT_SUCCESS(Status))
        {
            FrLdrHeapFree(BootDriverEntry->FilePath.Buffer, TAG_WLDR_NAME);
            FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
            return FALSE;
        }

        Status = RtlAppendUnicodeToString(&BootDriverEntry->FilePath, ServiceName);
        if (!NT_SUCCESS(Status))
        {
            FrLdrHeapFree(BootDriverEntry->FilePath.Buffer, TAG_WLDR_NAME);
            FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
            return FALSE;
        }

        Status = RtlAppendUnicodeToString(&BootDriverEntry->FilePath, L".sys");
        if (!NT_SUCCESS(Status))
        {
            FrLdrHeapFree(BootDriverEntry->FilePath.Buffer, TAG_WLDR_NAME);
            FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
            return FALSE;
        }
    }

    // Add registry path
    PathLength = (USHORT)(wcslen(RegistryPath) + wcslen(ServiceName))*sizeof(WCHAR) + sizeof(UNICODE_NULL);
    BootDriverEntry->RegistryPath.Length = 0;
    BootDriverEntry->RegistryPath.MaximumLength = PathLength;
    BootDriverEntry->RegistryPath.Buffer = FrLdrHeapAlloc(PathLength, TAG_WLDR_NAME);
    if (!BootDriverEntry->RegistryPath.Buffer)
    {
        FrLdrHeapFree(BootDriverEntry->FilePath.Buffer, TAG_WLDR_NAME);
        FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
        return FALSE;
    }

    Status = RtlAppendUnicodeToString(&BootDriverEntry->RegistryPath, RegistryPath);
    if (!NT_SUCCESS(Status))
    {
        FrLdrHeapFree(BootDriverEntry->RegistryPath.Buffer, TAG_WLDR_NAME);
        FrLdrHeapFree(BootDriverEntry->FilePath.Buffer, TAG_WLDR_NAME);
        FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
        return FALSE;
    }

    Status = RtlAppendUnicodeToString(&BootDriverEntry->RegistryPath, ServiceName);
    if (!NT_SUCCESS(Status))
    {
        FrLdrHeapFree(BootDriverEntry->RegistryPath.Buffer, TAG_WLDR_NAME);
        FrLdrHeapFree(BootDriverEntry->FilePath.Buffer, TAG_WLDR_NAME);
        FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
        return FALSE;
    }

    // Insert entry into the list
    if (!InsertInBootDriverList(BootDriverListHead, BootDriverEntry))
    {
        // It was already there, so delete our entry
        FrLdrHeapFree(BootDriverEntry->RegistryPath.Buffer, TAG_WLDR_NAME);
        FrLdrHeapFree(BootDriverEntry->FilePath.Buffer, TAG_WLDR_NAME);
        FrLdrHeapFree(BootDriverEntry, TAG_WLDR_BDE);
    }

    return TRUE;
}
