.PHONY:build clean uninstall_ubuntu

build:
	@mkdir -p build
	@cmake -B build
	@cmake --build build --target pm_tiny --target pm --target pm_sdk
	@cmake --install build

install_ubuntu: build
	@chmod a+x script/ubuntu.sh
	@./script/ubuntu.sh
	@cp -f build/_install/Release/bin/* /usr/local/bin/

uninstall_ubuntu:
	-@which pm >/dev/null 2>&1 && pm quit 2>/dev/null && systemctl disable pm_tiny.service
	@rm -rf /lib/systemd/system/pm_tiny.service
	@rm -rf /usr/local/bin/pm_tiny
	@rm -rf /usr/local/bin/pm
	@rm -rf /usr/local/pm_tiny
	@rm -rf /var/log/pm_tiny


clean:
	@rm -rf build
