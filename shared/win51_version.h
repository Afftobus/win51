// Version strings for resources and code. WIN51_VERSION_DOTTED (1.2.3.0) and WIN51_VERSION_NUM (1,2,3,0) are
// defined on the compiler command line from Directory.Build.props: rc.exe cannot take a quoted string there.
#pragma once

#define WIN51_STRINGIZE2(x) #x
#define WIN51_STRINGIZE(x) WIN51_STRINGIZE2(x)
#define WIN51_VERSION_STR WIN51_STRINGIZE(WIN51_VERSION_DOTTED)
