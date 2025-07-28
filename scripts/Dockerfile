FROM archlinux AS build_env

ARG EXTRA_PACKAGES=""
ARG AUR_EXTRA_PACKAGES=""

RUN pacman --disable-download-timeout --noconfirm -Syyu
RUN pacman --disable-download-timeout --noconfirm --needed -S \
    base-devel fish llvm cmake lld git make gcc clang python sudo python-intelhex fmt nlohmann-json pugixml cli11 boost $EXTRA_PACKAGES

RUN groupadd -r -g 5000 aur_user
RUN useradd aur_user -u 5000 -g 5000 --create-home
RUN echo "aur_user ALL=(ALL) NOPASSWD: /usr/bin/pacman" > "/etc/sudoers.d/allow_nobody_to_pacman"
RUN su aur_user -c "/bin/sh -c 'cd ~ && git clone https://aur.archlinux.org/yay.git && cd yay && makepkg --noconfirm -si'"
RUN su aur_user -c "/bin/sh -c 'yay -S --noconfirm jlink-software-and-documentation su-exec inja ftxui magic_enum $AUR_EXTRA_PACKAGES'"

RUN git config --global --add safe.directory /project

RUN mkdir /project
WORKDIR /project

FROM build_env AS build
ENTRYPOINT ["/project/kvasir/scripts/entrypoint.sh"]
