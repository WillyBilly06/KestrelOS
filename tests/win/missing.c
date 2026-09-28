/* missing - a program that calls something this system does not implement.
 *
 * The loader cannot refuse to start it: most programs import far more than
 * they call, and refusing on an unresolved import would turn "this program
 * uses one function we lack" into "this program does not run".  So the import
 * gets a stub, and the stub names the function if it is ever reached.
 *
 * This program reaches it deliberately.  It should print the line below, then
 * be stopped with the name of what it wanted. */
#include "winapi.h"

__declspec(dllimport) void WINAPI AFunctionThatIsNotImplemented(void);

int main(void) {
    printf("missing: about to call something that is not implemented\n");
    AFunctionThatIsNotImplemented();
    printf("missing: FAIL - the call returned, which it must not\n");
    return 1;
}
