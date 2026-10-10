#pragma once
// The one place to bump the version: the exe resources, the UI and the installer (DSM.iss) read it from here.
#define DSM_VERSION_MAJOR 1
#define DSM_VERSION_MINOR 0
#define DSM_VERSION_PATCH 1

#define DSM_STR2(x) #x
#define DSM_STR(x) DSM_STR2(x)
#define DSM_VERSION DSM_STR(DSM_VERSION_MAJOR) "." DSM_STR(DSM_VERSION_MINOR) "." DSM_STR(DSM_VERSION_PATCH)
