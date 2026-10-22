# Image for test-building obs-studio-plugin-ptz.spec on Fedora. Build and run it with
# packaging/fedora/docker-test.sh, which feeds it the current checkout.
ARG FEDORA_VERSION=rawhide
ARG BASE_IMAGE=registry.fedoraproject.org/fedora
FROM ${BASE_IMAGE}:${FEDORA_VERSION}

RUN dnf -y install --setopt=install_weak_deps=False \
        rpm-build rpmdevtools rpmlint fedora-review git-core tar gzip \
        glibc-langpack-en \
    && dnf clean all

# Build dependencies come from the spec itself, so the image stays in sync
COPY obs-studio-plugin-ptz.spec /tmp/obs-studio-plugin-ptz.spec
RUN dnf -y builddep --setopt=install_weak_deps=False /tmp/obs-studio-plugin-ptz.spec \
    && dnf clean all

RUN useradd -m builder
USER builder
WORKDIR /home/builder
RUN rpmdev-setuptree
COPY --chmod=755 build-in-container.sh /usr/local/bin/build-in-container.sh
ENTRYPOINT ["/usr/local/bin/build-in-container.sh"]
