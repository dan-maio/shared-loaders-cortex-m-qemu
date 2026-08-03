add_subdirectory(test1)   # demo app: dlopen/dlsym/call/dlclose mylib1/2, libcppsh, EH lib
add_subdirectory(test2)   # cross-module battery: c1/c2/cpp3, cross-call, lazy, EH, GOT dumps
add_subdirectory(test3)   # flat-fs battery: provision on-chip archive, load libs FROM FLASH
add_subdirectory(test4)   # GOT/PLT standard resolution demo: GOT[2], PLT0, PLTn walkthrough
