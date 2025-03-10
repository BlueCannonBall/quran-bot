# This file was auto-generated by Polybuild

ifndef OS
	OS := $(shell uname)
endif

obj_ext := .o
ifeq ($(OS),Windows_NT)
	obj_ext := .obj
	out_ext := .exe
endif

compiler := $(CXX)
compilation_flags := -Wall -std=c++17 -O3
libraries := -ldpp -lssl -lcrypto

default: quran-bot$(out_ext)
.PHONY: default

obj/main_0$(obj_ext): ./main.cpp ./Polyweb/polyweb.hpp ./Polyweb/Polynet/polynet.hpp ./Polyweb/Polynet/string.hpp ./Polyweb/Polynet/secure_sockets.hpp ./Polyweb/Polynet/smart_sockets.hpp ./Polyweb/string.hpp ./Polyweb/threadpool.hpp ./json.hpp ./system_instructions.hpp
	@printf '\033[1m[POLYBUILD]\033[0m Compiling $@ from $<...\n'
	@mkdir -p obj
	@$(compiler) -c $< $(compilation_flags) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished compiling $@ from $<!\n'

obj/string_0$(obj_ext): Polyweb/string.cpp Polyweb/string.hpp Polyweb/Polynet/string.hpp
	@printf '\033[1m[POLYBUILD]\033[0m Compiling $@ from $<...\n'
	@mkdir -p obj
	@$(compiler) -c $< $(compilation_flags) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished compiling $@ from $<!\n'

obj/client_0$(obj_ext): Polyweb/client.cpp Polyweb/polyweb.hpp Polyweb/Polynet/polynet.hpp Polyweb/Polynet/string.hpp Polyweb/Polynet/secure_sockets.hpp Polyweb/Polynet/smart_sockets.hpp Polyweb/string.hpp Polyweb/threadpool.hpp
	@printf '\033[1m[POLYBUILD]\033[0m Compiling $@ from $<...\n'
	@mkdir -p obj
	@$(compiler) -c $< $(compilation_flags) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished compiling $@ from $<!\n'

obj/polyweb_0$(obj_ext): Polyweb/polyweb.cpp Polyweb/polyweb.hpp Polyweb/Polynet/polynet.hpp Polyweb/Polynet/string.hpp Polyweb/Polynet/secure_sockets.hpp Polyweb/Polynet/smart_sockets.hpp Polyweb/string.hpp Polyweb/threadpool.hpp
	@printf '\033[1m[POLYBUILD]\033[0m Compiling $@ from $<...\n'
	@mkdir -p obj
	@$(compiler) -c $< $(compilation_flags) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished compiling $@ from $<!\n'

obj/websocket_0$(obj_ext): Polyweb/websocket.cpp Polyweb/polyweb.hpp Polyweb/Polynet/polynet.hpp Polyweb/Polynet/string.hpp Polyweb/Polynet/secure_sockets.hpp Polyweb/Polynet/smart_sockets.hpp Polyweb/string.hpp Polyweb/threadpool.hpp
	@printf '\033[1m[POLYBUILD]\033[0m Compiling $@ from $<...\n'
	@mkdir -p obj
	@$(compiler) -c $< $(compilation_flags) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished compiling $@ from $<!\n'

obj/server_0$(obj_ext): Polyweb/server.cpp Polyweb/polyweb.hpp Polyweb/Polynet/polynet.hpp Polyweb/Polynet/string.hpp Polyweb/Polynet/secure_sockets.hpp Polyweb/Polynet/smart_sockets.hpp Polyweb/string.hpp Polyweb/threadpool.hpp
	@printf '\033[1m[POLYBUILD]\033[0m Compiling $@ from $<...\n'
	@mkdir -p obj
	@$(compiler) -c $< $(compilation_flags) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished compiling $@ from $<!\n'

obj/polynet_0$(obj_ext): Polyweb/Polynet/polynet.cpp Polyweb/Polynet/polynet.hpp Polyweb/Polynet/string.hpp Polyweb/Polynet/secure_sockets.hpp
	@printf '\033[1m[POLYBUILD]\033[0m Compiling $@ from $<...\n'
	@mkdir -p obj
	@$(compiler) -c $< $(compilation_flags) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished compiling $@ from $<!\n'

obj/secure_sockets_0$(obj_ext): Polyweb/Polynet/secure_sockets.cpp Polyweb/Polynet/secure_sockets.hpp Polyweb/Polynet/polynet.hpp Polyweb/Polynet/string.hpp
	@printf '\033[1m[POLYBUILD]\033[0m Compiling $@ from $<...\n'
	@mkdir -p obj
	@$(compiler) -c $< $(compilation_flags) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished compiling $@ from $<!\n'

quran-bot$(out_ext): obj/main_0$(obj_ext) obj/string_0$(obj_ext) obj/client_0$(obj_ext) obj/polyweb_0$(obj_ext) obj/websocket_0$(obj_ext) obj/server_0$(obj_ext) obj/polynet_0$(obj_ext) obj/secure_sockets_0$(obj_ext)
	@printf '\033[1m[POLYBUILD]\033[0m Building $@...\n'
	@$(compiler) $^ $(static_libraries) $(compilation_flags) $(libraries) -o $@
	@printf '\033[1m[POLYBUILD]\033[0m Finished building $@!\n'

clean:
	@printf '\033[1m[POLYBUILD]\033[0m Deleting quran-bot$(out_ext) and obj...\n'
	@rm -rf quran-bot$(out_ext) obj
	@printf '\033[1m[POLYBUILD]\033[0m Finished deleting quran-bot$(out_ext) and obj!\n'
.PHONY: clean
