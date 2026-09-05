LIBGIT2 = vendor/libgit2/build/libgit2.a

prompt: main.c $(LIBGIT2)
	$(CC) -O2 -Ivendor/libgit2/include -Wall -Wextra -Wpedantic -Wconversion -Wshadow main.c $(LIBGIT2) -o prompt

debug: main.c $(LIBGIT2)
	$(CC) -g -O0 -Ivendor/libgit2/include main.c $(LIBGIT2) -o promptdbg

$(LIBGIT2):
	cmake -S vendor/libgit2 -B vendor/libgit2/build -DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_CLI=OFF \
		-DUSE_THREADS=OFF -DUSE_SSH=OFF -DUSE_HTTPS=OFF -DUSE_GSSAPI=OFF \
		-DUSE_NTLMCLIENT=OFF -DREGEX_BACKEND=builtin -DUSE_BUNDLED_ZLIB=ON \
		-DUSE_SHA1=CollisionDetection -DUSE_SHA256=builtin
	cmake --build vendor/libgit2/build --target libgit2package --parallel

clean:
	rm -rf prompt prompt promptdbg
