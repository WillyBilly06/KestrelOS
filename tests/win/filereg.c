/* filereg - files, directories and the registry.
 *
 * These are what an installer does, and between them they are most of why a
 * Windows program touches the system at all: put files somewhere, remember
 * where they went, find them again next time. */
#include "winapi.h"

static char buffer[512];

int main(void) {
    printf("filereg: files, directories and the registry\n");

    printf(" writing and reading a file\n");
    HANDLE h = CreateFileA("C:\\tmp\\filereg.txt", GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE, "CreateFileA created it");

    const char *text = "the quick brown fox\njumps over the lazy dog\n";
    DWORD put = 0;
    check(WriteFile(h, text, (DWORD)strlen(text), &put, NULL), "WriteFile succeeded");
    check(put == strlen(text), "it wrote every byte");
    check(CloseHandle(h), "CloseHandle accepted it");

    h = CreateFileA("C:\\tmp\\filereg.txt", GENERIC_READ, 0, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE, "and it opens again for reading");
    check(GetFileSize(h, NULL) == strlen(text), "the size is what was written");

    DWORD got = 0;
    memset(buffer, 0, sizeof buffer);
    check(ReadFile(h, buffer, sizeof buffer - 1, &got, NULL), "ReadFile succeeded");
    check(got == strlen(text), "it read every byte");
    check(!strcmp(buffer, text), "the contents came back unchanged");

    printf(" seeking\n");
    check(SetFilePointer(h, 4, NULL, FILE_BEGIN) == 4, "SetFilePointer moved to four");
    memset(buffer, 0, sizeof buffer);
    ReadFile(h, buffer, 5, &got, NULL);
    check(got == 5 && !memcmp(buffer, "quick", 5), "which is where \"quick\" starts");
    CloseHandle(h);

    printf(" opening what is not there\n");
    SetLastError(0);
    h = CreateFileA("C:\\tmp\\this-does-not-exist", GENERIC_READ, 0, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h == INVALID_HANDLE_VALUE, "a missing file is refused");
    check(GetLastError() == ERROR_FILE_NOT_FOUND, "and the reason is the right one");

    printf(" directories\n");
    RemoveDirectoryA("C:\\tmp\\fr-dir");
    check(CreateDirectoryA("C:\\tmp\\fr-dir", NULL), "CreateDirectoryA made one");
    check(GetFileAttributesA("C:\\tmp\\fr-dir") & FILE_ATTRIBUTE_DIRECTORY,
          "and it reports itself as a directory");

    for (int i = 0; i < 3; i++) {
        char name[128];
        sprintf(name, "C:\\tmp\\fr-dir\\file%d.dat", i);
        HANDLE f = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD n = 0;
        WriteFile(f, "xxxx", 4, &n, NULL);
        CloseHandle(f);
    }

    printf(" walking a directory with a wildcard\n");
    WIN32_FIND_DATAA found;
    HANDLE walk = FindFirstFileA("C:\\tmp\\fr-dir\\*.dat", &found);
    check(walk != INVALID_HANDLE_VALUE, "FindFirstFileA found something");
    int count = 0;
    int sizes_right = 1;
    do {
        count++;
        if (found.nFileSizeLow != 4) sizes_right = 0;
    } while (FindNextFileA(walk, &found));
    FindClose(walk);
    check(count == 3, "all three files were listed");
    if (count != 3) printf("       listed %d\n", count);
    check(sizes_right, "each was reported as four bytes");

    walk = FindFirstFileA("C:\\tmp\\fr-dir\\*.absent", &found);
    check(walk == INVALID_HANDLE_VALUE, "a pattern matching nothing finds nothing");

    printf(" moving and deleting\n");
    check(MoveFileA("C:\\tmp\\fr-dir\\file0.dat", "C:\\tmp\\fr-dir\\moved.dat"),
          "MoveFileA renamed one");
    check(GetFileAttributesA("C:\\tmp\\fr-dir\\moved.dat") != INVALID_FILE_ATTRIBUTES,
          "the new name exists");
    check(GetFileAttributesA("C:\\tmp\\fr-dir\\file0.dat") == INVALID_FILE_ATTRIBUTES,
          "the old one does not");
    check(DeleteFileA("C:\\tmp\\fr-dir\\moved.dat"), "DeleteFileA removed it");

    printf(" the registry\n");
    HKEY key = NULL;
    DWORD disposition = 0;
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\Kestrel\\FileRegTest");
    LONG r = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Kestrel\\FileRegTest", 0, NULL,
                             0, KEY_ALL_ACCESS, NULL, &key, &disposition);
    check(r == ERROR_SUCCESS, "RegCreateKeyExA made a key");
    check(disposition == 1, "and said it was new");

    const char *value = "C:\\program files\\example";
    check(RegSetValueExA(key, "InstallPath", 0, REG_SZ,
                         (const BYTE *)value, (DWORD)strlen(value) + 1) == ERROR_SUCCESS,
          "a string was written");

    DWORD number = 20260826;
    check(RegSetValueExA(key, "Build", 0, REG_DWORD,
                         (const BYTE *)&number, sizeof number) == ERROR_SUCCESS,
          "a number was written");
    RegCloseKey(key);

    printf(" and read back after closing\n");
    check(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Kestrel\\FileRegTest", 0,
                        KEY_ALL_ACCESS, &key) == ERROR_SUCCESS, "the key opens again");

    DWORD type = 0, len = sizeof buffer;
    memset(buffer, 0, sizeof buffer);
    check(RegQueryValueExA(key, "InstallPath", NULL, &type, (BYTE *)buffer, &len) == ERROR_SUCCESS,
          "the string came back");
    check(type == REG_SZ, "with the type it was written as");
    check(!strcmp(buffer, value), "and the same text");

    DWORD read_number = 0;
    len = sizeof read_number;
    check(RegQueryValueExA(key, "Build", NULL, &type, (BYTE *)&read_number, &len) == ERROR_SUCCESS,
          "the number came back");
    check(type == REG_DWORD && read_number == 20260826, "unchanged");

    printf(" listing what is in a key\n");
    int values_seen = 0;
    for (DWORD i = 0; ; i++) {
        char name[64];
        DWORD name_len = sizeof name, vlen = sizeof buffer;
        if (RegEnumValueA(key, i, name, &name_len, NULL, &type, (BYTE *)buffer, &vlen) != ERROR_SUCCESS)
            break;
        values_seen++;
    }
    check(values_seen == 2, "both values were listed");

    check(RegDeleteValueA(key, "Build") == ERROR_SUCCESS, "one was deleted");
    len = sizeof read_number;
    check(RegQueryValueExA(key, "Build", NULL, &type, (BYTE *)&read_number, &len) != ERROR_SUCCESS,
          "and is gone");
    RegCloseKey(key);
    check(RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\Kestrel\\FileRegTest") == ERROR_SUCCESS,
          "the key was deleted");

    return report("filereg");
}
