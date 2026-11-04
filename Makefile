clangd:
	make -C linux/ LLVM=1 olddefconfig
	make -C linux/ LLVM=1 -j18
	make -C linux/ LLVM=1 compile_commands.json
pi:
	url="https://github.com/earendil-works/pi/releases/download/v1.5.0/pi-linux-x64.tar.gz"; \
	curl -fL "$$url" | tar -xz -C ./
	cd pi && ./pi install -l git:github.com/gitawego/pi-lsp
