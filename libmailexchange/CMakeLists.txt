project(libpop3)
find_package(Boost 1.48 COMPONENTS regex signals system locale)

add_library(mailexchange pop3.cpp smtp.cpp)
target_link_libraries(mailexchange ${Boost_LIBRARIES})

if(ENABLE_STATIC)

	set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})
	find_package(ICU )
	if(ICU_FOUND)
		target_link_libraries(mailexchange ${ICU_LIBRARIES})
	endif()
endif()

if( ENABLE_TEST )
add_definitions(-DDEBUG)
add_executable(pop3test pop3test.cpp)
target_link_libraries(pop3test mailexchange ${CMAKE_THREAD_LIBS_INIT} )

add_executable(smtptest smtptest.cpp)
target_link_libraries(smtptest mailexchange ${CMAKE_THREAD_LIBS_INIT} )

endif()