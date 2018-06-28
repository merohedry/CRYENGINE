option(PROJECT_CRYENGINE_GAMETEST1 "Build GameTest1 sample project" ON)
if (NOT PROJECT_CRYENGINE_GAMETEST1)
	return()
endif()

add_subdirectory(GameDll)
