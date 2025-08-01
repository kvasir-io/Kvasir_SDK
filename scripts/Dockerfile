FROM archlinux AS build_env

ARG EXTRA_PACKAGES=""
ARG AUR_EXTRA_PACKAGES=""

RUN pacman --disable-download-timeout --noconfirm -Syyu && \
    pacman --disable-download-timeout --noconfirm --needed -S \
        base-devel fish llvm cmake lld git make gcc clang python sudo python-intelhex \
        fmt nlohmann-json pugixml cli11 boost go $EXTRA_PACKAGES && \
    groupadd -r -g 5000 aur_user && \
    useradd aur_user -u 5000 -g 5000 --create-home && \
    echo "aur_user ALL=(ALL) NOPASSWD: /usr/bin/pacman" > "/etc/sudoers.d/allow_nobody_to_pacman" && \
    echo "MAKEFLAGS=\"-j$(nproc)\"" >> /etc/makepkg.conf && \
    echo "OPTIONS=(strip !docs !libtool staticlibs !debug !check)" >> /etc/makepkg.conf && \
    su aur_user -c "cd ~ && git clone --depth 1 https://aur.archlinux.org/yay.git && cd yay && makepkg --noconfirm -si" && \
    su aur_user -c "yay -S --noconfirm --nodeps --needed jlink-software-and-documentation su-exec inja ftxui magic_enum $AUR_EXTRA_PACKAGES" && \
    yay -Rns --noconfirm $(yay -Qtdq) 2>/dev/null || true && \
    yay -Scc --noconfirm && \
    go clean -cache -modcache -testcache -fuzzcache 2>/dev/null || true && \
    rm -rf /home/aur_user/.cache/* /home/aur_user/yay /home/aur_user/.config/yay \
           /home/aur_user/.local/share/go /root/.cache/go-build /root/go \
           /tmp/* /var/tmp/* /var/cache/pacman/pkg/* /var/lib/pacman/sync/* \
           /usr/share/man/* /usr/share/doc/* /usr/share/info/* \
           /usr/share/locale/*/LC_MESSAGES/* /usr/share/i18n/* \
           /var/cache/go/* /usr/local/go/pkg/mod \
           /usr/share/gtk-doc/* /usr/share/help/* \
           /var/lib/systemd/* /var/log/* && \
    find /usr/bin -name "*-debug" -delete 2>/dev/null || true && \
    pacman -Scc --noconfirm && \
    git config --global --add safe.directory /project && \
    mkdir /project

WORKDIR /project

FROM build_env AS build
ENTRYPOINT ["/project/kvasir/scripts/entrypoint.sh"]