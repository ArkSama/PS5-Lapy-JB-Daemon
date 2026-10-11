#!/usr/bin/env bash
# Automated Build Script via Docker

IMAGE_NAME="ps5-payload-sdk"

function check_sdk() {
    if [[ "$(docker images -q $IMAGE_NAME 2> /dev/null)" == "" ]]; then
        echo "!!! SDK image not found. Please run ./build.sh first."
        exit 1
    fi
    echo "--- Current SDK Version (Last Commit) ---"
    docker run --rm $IMAGE_NAME git -C /opt/ps5-payload-sdk-source log -1
}

function update_sdk() {
    echo "--- Updating SDK (Forcing rebuild of Docker image) ---"
    docker build --no-cache -t $IMAGE_NAME -f Dockerfile.sdk .
    if [ $? -eq 0 ]; then
        echo "--- SDK updated successfully. ---"
    else
        echo "!!! SDK update FAILED!"
        exit 1
    fi
}

function build_elf() {
    # 1. Build the SDK Docker image if it doesn't exist
    if [[ "$(docker images -q $IMAGE_NAME 2> /dev/null)" == "" ]]; then
        echo "--- Docker image $IMAGE_NAME not found. Building... ---"
        docker build -t $IMAGE_NAME -f Dockerfile.sdk .
        if [ $? -ne 0 ]; then
            echo "!!! Docker image build FAILED!"
            exit 1
        fi
        echo "--- Docker image built successfully. ---"
    fi

    # 2. Build the native ELF via Docker
    echo "--- Building lapy_jb_daemon.elf via Docker ---"
    docker run --rm -v "$(pwd)":/src -w /src $IMAGE_NAME make clean all

    if [ $? -ne 0 ]; then
        echo "!!! Build FAILED! Check source errors."
        exit 1
    fi

    echo "--- Build successful! lapy_jb_daemon.elf generated. ---"
}

# Command routing
case "$1" in
    check-sdk)
        check_sdk
        ;;
    update-sdk)
        update_sdk
        ;;
    *)
        build_elf
        ;;
esac
