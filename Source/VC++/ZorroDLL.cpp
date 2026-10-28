///////////////////////////////////////////////////////
// Common code for DLL-based Zorro strategies
///////////////////////////////////////////////////////
//#include "stdafx.h"
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#define ZORRO_CPP
#include <zorro.h>

#pragma warning( push )
#pragma warning( disable: 4100 )
BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{
    return TRUE;
}
#pragma warning( pop )

////////////////////////////////////////////////////////
// Create a list of function pointers
#define F(x) (*x)
#define F0(x) (*x##0)
#define F1(x) (*x##1)
#define F2(x) (*x##2)
#define F3(x) (*x##3)
#define C
#include <functions.h>

GLOBALS *g;

DLLFUNC int zorro(GLOBALS* Globals)
{
	g = Globals;
	int N = 0;
// Populate the list of function pointers
#pragma warning( disable: 4005 )
#define F(x) (intptr_t&)x = g->Functions[N++];
#define F0(x) (intptr_t&)x##0 = g->Functions[N++];
#define F1(x) (intptr_t&)x##1 = g->Functions[N++];
#define F2(x) (intptr_t&)x##2 = g->Functions[N++];
#define F3(x) (intptr_t&)x##3 = g->Functions[N++];
#include <func_list.h>

	return SCRIPT_VERSION;
}

void _printf(const char* Format, ...)
{
	char Text[1024];
	va_list arglist;
	va_start(arglist,Format);
	vsnprintf_s(Text,1024,1023,Format,arglist);
	va_end(arglist);
	if(*Text == '#')
		print(TO_LOG,"%s",Text+1);
	else if(*Text == '&')
		print(TO_DIAG,"%s",Text+1);
	else
		print(TO_WINDOW,"%s",Text);
}

