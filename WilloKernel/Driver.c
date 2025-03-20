#include <ntddk.h>
#include <ntddkbd.h>
#include <ntstrsafe.h>

#define NUM_FILES 8
#define KEY_BREAK 0x80
#define SHIFT_MAKE 0x2A


typedef struct _FILE_BUFFER {
    PCHAR Buffer;
    SIZE_T BufferSize;
} FILE_BUFFER, * PFILE_BUFFER;

FILE_BUFFER g_FileBuffers[NUM_FILES] = { 0 };


VOID DriverUnload(PDRIVER_OBJECT DriverObject);
NTSTATUS KeyboardFilterReadDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS KeyboardFilterPassThrough(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS KeyboardFilterReadComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context);

NTSTATUS ReadTextFileFromKernel(IN PCWSTR FileName, OUT PCHAR* Buffer, OUT SIZE_T* BufferSize);
NTSTATUS CreateDirectoryIfNotExist(PUNICODE_STRING DirectoryPath);
NTSTATUS CreateFileIfNotExist(PUNICODE_STRING FilePath);

NTSTATUS ReloadAllFiles();

UCHAR ConvertAsciiToScancode(char ch);
BOOLEAN RequiresShift(char ch);

VOID ReloadAllFilesWorker(PVOID Context);

PDEVICE_OBJECT g_FilterDeviceObject = NULL;
PDEVICE_OBJECT g_LowerKeyboardDeviceObject = NULL;

typedef struct _RELOAD_WORK_ITEM {
    WORK_QUEUE_ITEM WorkItem;
} RELOAD_WORK_ITEM, * PRELOAD_WORK_ITEM;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    DbgPrint("==================== Willo KEYBOARD FILTER STARTING ====================\n");

    UNREFERENCED_PARAMETER(RegistryPath);
    NTSTATUS status;
    UNICODE_STRING targetDeviceName;
    PFILE_OBJECT fileObject = NULL;
    PDEVICE_OBJECT keyboardDeviceObject = NULL;

    DbgPrint("Willo: Keyboard filter driver loading...\n");

    //
    // Create the folder structure.
    //
    {
        UNICODE_STRING dirWilloProject, dirCopySources, dirScreenshots, dirGptResults, dirSettings;
        RtlInitUnicodeString(&dirWilloProject, L"\\??\\C:\\WilloProject");
        RtlInitUnicodeString(&dirCopySources, L"\\??\\C:\\WilloProject\\copy_sources");
        RtlInitUnicodeString(&dirScreenshots, L"\\??\\C:\\WilloProject\\screenshots");
        RtlInitUnicodeString(&dirGptResults, L"\\??\\C:\\WilloProject\\gpt_results");
        RtlInitUnicodeString(&dirSettings, L"\\??\\C:\\WilloProject\\settings");

        CreateDirectoryIfNotExist(&dirWilloProject);
        CreateDirectoryIfNotExist(&dirCopySources);
        CreateDirectoryIfNotExist(&dirScreenshots);
        CreateDirectoryIfNotExist(&dirGptResults);
        CreateDirectoryIfNotExist(&dirSettings);
    }

    //
    // Create text files F1.txt ~ F8.txt under "copy_sources" if they do not exist.
    //
    {
        WCHAR filePathBuffer[256];
        UNICODE_STRING filePath;
        for (int i = 1; i <= NUM_FILES; i++) {
            RtlStringCchPrintfW(filePathBuffer, 256, L"\\??\\C:\\WilloProject\\copy_sources\\F%d.txt", i);
            RtlInitUnicodeString(&filePath, filePathBuffer);
            CreateFileIfNotExist(&filePath);
        }
    }

    //
    // Read all F1.txt ~ F8.txt into memory once during DriverEntry.
    //
    {
        WCHAR filePathBuffer[256];
        for (int i = 0; i < NUM_FILES; i++) {
            RtlStringCchPrintfW(filePathBuffer, 256, L"\\??\\C:\\WilloProject\\copy_sources\\F%d.txt", i + 1);
            status = ReadTextFileFromKernel(filePathBuffer, &g_FileBuffers[i].Buffer, &g_FileBuffers[i].BufferSize);
            if (!NT_SUCCESS(status)) {
                DbgPrint("Willo: Failed to read file F%d.txt, status: 0x%X\n", i + 1, status);
                g_FileBuffers[i].Buffer = NULL;
                g_FileBuffers[i].BufferSize = 0;
            }
            else {
                DbgPrint("Willo: Successfully loaded F%d.txt into memory\n", i + 1);
            }
        }
    }

    // Initialize the target lower device name.
    RtlInitUnicodeString(&targetDeviceName, L"\\Device\\KeyboardClass0");

    // Get the pointer to the lower device object (also obtains a FILE_OBJECT, which is later dereferenced)
    status = IoGetDeviceObjectPointer(&targetDeviceName, FILE_READ_DATA, &fileObject, &keyboardDeviceObject);
    if (!NT_SUCCESS(status)) {
        DbgPrint("Willo: Failed to get keyboard device object pointer. Status: 0x%X\n", status);
        return status;
    }
    DbgPrint("Willo: IoGetDeviceObjectPointer Status: 0x%X\n", status);
    ObDereferenceObject(fileObject);

    // Create the filter device object.
    status = IoCreateDevice(DriverObject, 0, NULL, FILE_DEVICE_KEYBOARD, 0, FALSE, &g_FilterDeviceObject);
    if (!NT_SUCCESS(status)) {
        DbgPrint("Willo: Failed to create filter device. Status: 0x%X\n", status);
        return status;
    }
    DbgPrint("Willo: IoCreateDevice Status: 0x%X\n", status);

    g_FilterDeviceObject->Flags |= (keyboardDeviceObject->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE));
    g_FilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    // Attach the filter device to the lower device stack.
    g_LowerKeyboardDeviceObject = IoAttachDeviceToDeviceStack(g_FilterDeviceObject, keyboardDeviceObject);
    if (!g_LowerKeyboardDeviceObject) {
        IoDeleteDevice(g_FilterDeviceObject);
        DbgPrint("Willo: Failed to attach to keyboard device stack.\n");
        return STATUS_UNSUCCESSFUL;
    }

    g_FilterDeviceObject->DeviceType = keyboardDeviceObject->DeviceType;
    g_FilterDeviceObject->Characteristics = keyboardDeviceObject->Characteristics;

    // Set up the driver’s major function handlers.
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        switch (i) {
        case IRP_MJ_READ:
            DriverObject->MajorFunction[i] = KeyboardFilterReadDispatch;
            break;
        default:
            DriverObject->MajorFunction[i] = KeyboardFilterPassThrough;
            break;
        }
    }
    DriverObject->DriverUnload = DriverUnload;

    DbgPrint("Willo: Current IRQL: %d\n", KeGetCurrentIrql());

    DbgPrint("Willo: Keyboard filter driver loaded successfully.\n");
    return STATUS_SUCCESS;
}

NTSTATUS KeyboardFilterReadDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);

    // For IRP_MJ_READ, set a completion routine before passing it down.
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, KeyboardFilterReadComplete, NULL, TRUE, TRUE, TRUE);
    return IoCallDriver(g_LowerKeyboardDeviceObject, Irp);
}

NTSTATUS KeyboardFilterPassThrough(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    // Pass all other IRPs directly to the lower driver.
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(g_LowerKeyboardDeviceObject, Irp);
}

VOID ReloadAllFilesWorker(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    // Call the heavy file reload operation.
    ReloadAllFiles();

    // Free the memory allocated for the work item.
    ExFreePoolWithTag(Context, 'wldR');
}

NTSTATUS KeyboardFilterReadComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);

    // Check if the lower driver succeeded and SystemBuffer contains data.
    if (NT_SUCCESS(Irp->IoStatus.Status) && Irp->AssociatedIrp.SystemBuffer != NULL)
    {
        ULONG readBytes = (ULONG)Irp->IoStatus.Information;
        ULONG count = readBytes / sizeof(KEYBOARD_INPUT_DATA);
        PKEYBOARD_INPUT_DATA keyboardData = (PKEYBOARD_INPUT_DATA)Irp->AssociatedIrp.SystemBuffer;

        for (ULONG i = 0; i < count; i++)
        {
            DbgPrint("Willo: Key event (Complete): MakeCode = 0x%x, Flags = 0x%x\n",
                keyboardData[i].MakeCode, keyboardData[i].Flags);

            // When F9 is pressed, offload the reload work.
            if (keyboardData[i].MakeCode == 0x43)
            {
                PRELOAD_WORK_ITEM reloadWorkItem = (PRELOAD_WORK_ITEM)
                ExAllocatePoolWithTag(NonPagedPool, sizeof(RELOAD_WORK_ITEM), 'wldR');
                if (reloadWorkItem)
                {
                    ExInitializeWorkItem(&reloadWorkItem->WorkItem, ReloadAllFilesWorker, reloadWorkItem);
                    ExQueueWorkItem(&reloadWorkItem->WorkItem, CriticalWorkQueue);
                    DbgPrint("Willo: Queued work item to reload files\n");
                }
                else
                {
                    DbgPrint("Willo: Failed to allocate work item for reloading files\n");
                }
            }

            // F1～F8 key handling remains the same.
            if (keyboardData[i].MakeCode >= 0x3B && keyboardData[i].MakeCode <= 0x42)
            {
                int fileIndex = keyboardData[i].MakeCode - 0x3B; // F1ならindex 0、F2ならindex 1、...
                if (g_FileBuffers[fileIndex].Buffer != NULL)
                {
                    PCHAR fileContent = g_FileBuffers[fileIndex].Buffer;
                    ULONG fileLength = (ULONG)strlen(fileContent);
                    // Each character may result in 4 events if shift is required, 2 otherwise.
                    ULONG worstCaseInjectionEvents = fileLength * 4;
                    ULONG newCount = count - 1 + worstCaseInjectionEvents;
                    ULONG newSize = newCount * sizeof(KEYBOARD_INPUT_DATA);

                    PKEYBOARD_INPUT_DATA newBuffer = (PKEYBOARD_INPUT_DATA)
                        ExAllocatePoolWithTag(NonPagedPool, newSize, 'tksf');
                    if (newBuffer == NULL)
                    {
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }

                    // Copy events before the F-key event.
                    if (i > 0)
                    {
                        RtlCopyMemory(newBuffer, keyboardData, i * sizeof(KEYBOARD_INPUT_DATA));
                    }

                    DbgPrint("Willo: Injecting keystrokes from F%d.txt\n", fileIndex + 1);

                    ULONG baseFlags = keyboardData[i].Flags & ~KEY_BREAK;
                    ULONG index = i;  // New buffer insertion start

                    for (ULONG j = 0; j < fileLength; j++)
                    {
                        char ch = fileContent[j];
                        UCHAR scancode = ConvertAsciiToScancode(ch);

                        if (scancode == 0)
                        {
                            DbgPrint("Willo: Skipping unsupported character: %c\n", ch);
                            continue;
                        }

                        if (RequiresShift(ch))
                        {
                            // Inject shift + key events.
                            newBuffer[index].MakeCode = SHIFT_MAKE;
                            newBuffer[index].Flags = baseFlags;  // Make event
                            DbgPrint("Willo: Injected Shift Make for %c\n", ch);
                            index++;

                            newBuffer[index].MakeCode = scancode;
                            newBuffer[index].Flags = baseFlags;  // Make event
                            DbgPrint("Willo: Injected Make for %c, scancode=0x%x\n", ch, scancode);
                            index++;

                            newBuffer[index].MakeCode = scancode;
                            newBuffer[index].Flags = baseFlags | KEY_BREAK;  // Break event
                            DbgPrint("Willo: Injected Break for %c, scancode=0x%x\n", ch, scancode);
                            index++;

                            newBuffer[index].MakeCode = SHIFT_MAKE;
                            newBuffer[index].Flags = baseFlags | KEY_BREAK;  // Break event
                            DbgPrint("Willo: Injected Shift Break for %c\n", ch);
                            index++;
                        }
                        else
                        {
                            // Inject key press/release events.
                            newBuffer[index].MakeCode = scancode;
                            newBuffer[index].Flags = baseFlags;  // Make event
                            DbgPrint("Willo: Injected Make for %c, scancode=0x%x\n", ch, scancode);
                            index++;

                            newBuffer[index].MakeCode = scancode;
                            newBuffer[index].Flags = baseFlags | KEY_BREAK;  // Break event
                            DbgPrint("Willo: Injected Break for %c, scancode=0x%x\n", ch, scancode);
                            index++;
                        }
                    }

                    // Copy events after the F-key event.
                    if (i + 1 < count)
                    {
                        RtlCopyMemory(&newBuffer[index],
                            &keyboardData[i + 1],
                            (count - i - 1) * sizeof(KEYBOARD_INPUT_DATA));
                    }

                    ULONG injectedEvents = index - i;
                    newCount = i + injectedEvents + (count - i - 1);
                    newSize = newCount * sizeof(KEYBOARD_INPUT_DATA);
                    DbgPrint("Willo: Completed key injection for F%d, newCount=%lu\n", fileIndex + 1, newCount);

                    // Update the IRP.
                    Irp->IoStatus.Information = newSize;
                    Irp->AssociatedIrp.SystemBuffer = newBuffer;

                    break; // Only process one F-key event.
                }
            }
        }
    }

    if (Irp->PendingReturned)
    {
        IoMarkIrpPending(Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    return STATUS_CONTINUE_COMPLETION;
}


NTSTATUS ReloadAllFiles()
{
    NTSTATUS status;
    WCHAR filePathBuffer[256];
    PCHAR newBuffer;
    SIZE_T newBufferSize;

    for (int i = 0; i < NUM_FILES; i++) {
        // ファイルパスを作成
        RtlStringCchPrintfW(filePathBuffer, 256, L"\\??\\C:\\WilloProject\\copy_sources\\F%d.txt", i + 1);

        // 既存のバッファを解放
        if (g_FileBuffers[i].Buffer) {
            ExFreePoolWithTag(g_FileBuffers[i].Buffer, 'FBuf');
            g_FileBuffers[i].Buffer = NULL;
            g_FileBuffers[i].BufferSize = 0;
        }

        // 新しいデータを読み込む
        status = ReadTextFileFromKernel(filePathBuffer, &newBuffer, &newBufferSize);
        if (!NT_SUCCESS(status)) {
            DbgPrint("Willo: Failed to reload file F%d.txt, status: 0x%X\n", i + 1, status);
            continue;
        }

        // バッファを更新
        g_FileBuffers[i].Buffer = newBuffer;
        g_FileBuffers[i].BufferSize = newBufferSize;
        DbgPrint("Willo: Successfully reloaded F%d.txt into memory\n", i + 1);
    }

    return STATUS_SUCCESS;
}

// Helper function: Convert an ASCII character to a keyboard scancode.
UCHAR ConvertAsciiToScancode(char ch)
{
    // Convert lowercase letters to uppercase.
    if (ch >= 'a' && ch <= 'z')
        ch -= 32;

    switch (ch)
    {
        // Letters
    case 'A': return 0x1E;
    case 'B': return 0x30;
    case 'C': return 0x2E;
    case 'D': return 0x20;
    case 'E': return 0x12;
    case 'F': return 0x21;
    case 'G': return 0x22;
    case 'H': return 0x23;
    case 'I': return 0x17;
    case 'J': return 0x24;
    case 'K': return 0x25;
    case 'L': return 0x26;
    case 'M': return 0x32;
    case 'N': return 0x31;
    case 'O': return 0x18;
    case 'P': return 0x19;
    case 'Q': return 0x10;
    case 'R': return 0x13;
    case 'S': return 0x1F;
    case 'T': return 0x14;
    case 'U': return 0x16;
    case 'V': return 0x2F;
    case 'W': return 0x11;
    case 'X': return 0x2D;
    case 'Y': return 0x15;
    case 'Z': return 0x2C;

        // Digits
    case '0': return 0x0B;
    case '1': return 0x02;
    case '2': return 0x03;
    case '3': return 0x04;
    case '4': return 0x05;
    case '5': return 0x06;
    case '6': return 0x07;
    case '7': return 0x08;
    case '8': return 0x09;
    case '9': return 0x0A;

    // Space and newline
    case ' ': return 0x39;   // Space bar
    case '\n': return 0x1C;  // Enter key

    // Punctuation and symbols
    case ';':
    case ':': return 0x27;   // Semicolon/colon key
    case '\'':
    case '\"': return 0x28;  // Apostrophe/double quote key
    case ',': return 0x33;   // Comma
    case '.': return 0x34;   // Period
    case '!': return 0x02;   // Underlying key for '1'
    case '@': return 0x03;   // Underlying key for '2'
    case '#': return 0x04;   // Underlying key for '3'
    case '$': return 0x05;   // Underlying key for '4'
    case '/': return 0x35;   // Slash
    case '-': return 0x0C;   // Dash
    case '~': return 0x29;   // Underlying key for grave accent (`) 
    case '=': return 0x0D;   // Equal sign
    case '{':              // Left curly brace (same as open bracket)
    case '[': return 0x1A;
    case '}':              // Right curly brace (same as close bracket)
    case ']': return 0x1B;
    case '*': return 0x09;   // Underlying key for '8'
    default:   return 0;    // Unsupported character.
    }
}

BOOLEAN RequiresShift(char ch)
{
    // 大文字の場合はシフトが必要
    if (ch >= 'A' && ch <= 'Z')
        return TRUE;

    // 一部の記号はシフトを伴う（US配列の例）
    switch (ch)
    {
    case '!':
    case '@':
    case '#':
    case '$':
    case '%':
    case '^':
    case '&':
    case '*':
    case '(':
    case ')':
    case '_':
    case '+':
        return TRUE;
    default:
        return FALSE;
    }
}


NTSTATUS ReadTextFileFromKernel(IN PCWSTR FileName, OUT PCHAR* Buffer, OUT SIZE_T* BufferSize)
{
    HANDLE fileHandle;
    OBJECT_ATTRIBUTES objAttr;
    UNICODE_STRING filePath;
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;
    PVOID fileBuffer;
    ULONG fileLength;

    DbgPrint("Willo: Try to read file: %ws\n", FileName);

    // Convert the file path to UNICODE_STRING.
    RtlInitUnicodeString(&filePath, FileName);
    InitializeObjectAttributes(&objAttr, &filePath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    // Open the file.
    status = ZwCreateFile(&fileHandle, GENERIC_READ, &objAttr, &ioStatus, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(status)) {
        DbgPrint("Willo: Failed to open file: %wZ, status: 0x%X\n", &filePath, status);
        return status;
    }
    DbgPrint("Willo: Successfully opened file\n");

    // Get the file size.
    FILE_STANDARD_INFORMATION fileInfo;
    status = ZwQueryInformationFile(fileHandle, &ioStatus, &fileInfo, sizeof(fileInfo), FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        DbgPrint("Willo: Failed to get file size, status: 0x%X\n", status);
        ZwClose(fileHandle);
        return status;
    }

    fileLength = fileInfo.EndOfFile.LowPart;
    *BufferSize = fileLength;
    DbgPrint("Willo: File size: %lu bytes\n", fileLength);

    // Allocate a buffer from NonPagedPoolNx.
    fileBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, fileLength + 1, 'TxtF');
    if (!fileBuffer) {
        DbgPrint("Willo: Failed to allocate memory for file buffer.\n");
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    DbgPrint("Willo: Memory allocated for file buffer\n");

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    // Read the file.
    status = ZwReadFile(fileHandle, NULL, NULL, &event, &ioStatus, fileBuffer, fileLength, NULL, NULL);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }
    if (!NT_SUCCESS(status)) {
        DbgPrint("Willo: ZwReadFile failed, status: 0x%X\n", status);
        ExFreePoolWithTag(fileBuffer, 'TxtF');
        ZwClose(fileHandle);
        return status;
    }

    // Null-terminate the buffer.
    ((PCHAR)fileBuffer)[fileLength] = '\0';
    *Buffer = (PCHAR)fileBuffer;

    ZwClose(fileHandle);
    return STATUS_SUCCESS;
}

//
// Helper: Create a directory if it does not exist.
//
NTSTATUS CreateDirectoryIfNotExist(PUNICODE_STRING DirectoryPath)
{
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioStatus;
    HANDLE handle;
    InitializeObjectAttributes(&objAttr, DirectoryPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    NTSTATUS status = ZwCreateFile(&handle,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
        &objAttr,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN_IF,
        FILE_DIRECTORY_FILE,
        NULL,
        0);
    if (NT_SUCCESS(status)) {
        ZwClose(handle);
    }
    return status;
}

//
// Helper: Create a file (empty) if it does not exist.
//
NTSTATUS CreateFileIfNotExist(PUNICODE_STRING FilePath)
{
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioStatus;
    HANDLE handle;
    InitializeObjectAttributes(&objAttr, FilePath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    NTSTATUS status = ZwCreateFile(&handle,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE,
        &objAttr,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OPEN_IF,
        0,
        NULL,
        0);
    if (NT_SUCCESS(status)) {
        ZwClose(handle);
    }
    return status;
}

VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("Willo: Unloading keyboard filter driver...\n");

    // Free the stored file buffers.
    for (int i = 0; i < NUM_FILES; i++) {
        if (g_FileBuffers[i].Buffer != NULL) {
            ExFreePoolWithTag(g_FileBuffers[i].Buffer, 'TxtF');
            g_FileBuffers[i].Buffer = NULL;
        }
    }

    if (g_FilterDeviceObject) {
        IoDetachDevice(g_LowerKeyboardDeviceObject);
        IoDeleteDevice(g_FilterDeviceObject);
    }

    DbgPrint("Willo: Keyboard filter driver unloaded successfully.\n");
}
