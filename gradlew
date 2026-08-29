#!/bin/sh

set -e

DIRNAME=$(cd "$(dirname "$0")" && pwd)

if [ ! -f "$DIRNAME/gradle/wrapper/gradle-wrapper.jar" ]; then
    echo "Downloading Gradle 8.5..."
    mkdir -p "$DIRNAME/gradle/wrapper"
    curl -sS -L "https://services.gradle.org/distributions/gradle-8.5-bin.zip" -o "$DIRNAME/gradle-8.5-bin.zip"
    unzip -q "$DIRNAME/gradle-8.5-bin.zip" -d "$DIRNAME/gradle"
    mv "$DIRNAME/gradle/gradle-8.5"/* "$DIRNAME/gradle"
    rm -rf "$DIRNAME/gradle/gradle-8.5" "$DIRNAME/gradle-8.5-bin.zip"
fi

exec "$DIRNAME/gradle/bin/gradle" "$@"
