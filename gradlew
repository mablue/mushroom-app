#!/bin/sh

#
# Copyright 2015 the original author or authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

##############################################################################
##
##  Gradle start up script for UN*X
##
##############################################################################

# Attempt to set APP_HOME
# Resolve links: $0 may be a link
app_path=$0

# Need this for daisy-chained symlinks.
while
    APP_HOME=${app_path%"${app_path##*/}"}  # leaves a trailing /; empty if no leading path
    [ -h "$app_path" ]
do
    ls=$( ls -ld "$app_path" )
    link=${ls#*' -> '}
    case $link in             #(
      /*)   app_path=$link ;; #(
      *)    app_path=$APP_HOME$link ;;
    esac
done

APP_HOME=$( cd "${APP_HOME%.*}" && pwd -P ) || exit

APP_NAME="Gradle"
APP_BASE_NAME=${0##*/}
export APP_HOME
export APP_NAME
export APP_BASE_NAME

# Add default JVM options here. You can also use JAVA_OPTS and GRADLE_OPTS to pass JVM options to this script.
DEFAULT_JVM_OPTS='" -Xmx64m" "-Xms64m"'

# Use the maximum available, or set MAX_FD != "unlimited" if you encounter problems.
if ! expr "$MAX_FD" : '[0-9]\+$' > /dev/null; then
    MAX_FD=maximum
fi

warn () {
    echo "$*" >&2
} >&2

die () {
    echo
    echo "$*"
    echo
    exit 1
} >&2

# OS specific support (must be 'true' or 'false').
darwin=false
msys=false
cygwin=false
mingw=false
case "$( uname )" in                #(
  Darwin* )         darwin=true  ;; #(
  MINGW* )          mingw=true   ;; #(
  MSYS* )           msys=true    ;; #(
  CYGWIN* )         cygwin=true  ;; #(
  Linux* )          linux=true   ;;
esac

# Increase the maximum file descriptors if we can.
if ! "$cygwin" && ! "$darwin" && ! "$mingw" && ! "$msys" ; then
    case $MAX_FD in #(
      max*)
        # In POSIX sh, ulimit -H is undefined. That's why the result is checked to see if it worked.
        # shellcheck disable=SC3045
        MAX_FD=$( ulimit -H -n ) ||
            warn "Could not query maximum file descriptor limit"
    esac
    case $MAX_FD in  #(
      '' | soft) :;; #(
      *)
        # shellcheck disable=SC3045
        ulimit -n "$MAX_FD" ||
            warn "Could not set maximum file descriptor limit to $MAX_FD"
    esac
fi

# Collect all arguments for the java command; all will be passed to the Gradle daemon via the
# "-Dorg.gradle.appname" JVM option to let the Gradle daemon fork itself as a child of this
# script.
for arg in "$@"
do
    if
      case $arg in                                #(
        -*) false ;;                            # don't match the set variable in the case.
        /) ;
         { echo "$0": execution not found in $PATH; } 2>/dev/null
            exit 127
            ;;
        *)  true  ;;
      esac
    then
        arg=$( cygpath --unix "$arg" )
    fi
    # Roll the args list so the last arg is preceding the arg being added
    set -- "$@" "$arg"
done

# Escape application name
if [ -n "${APP_HOME}" ] ; then
    APP_HOME=$( cygpath --path --mixed "$APP_HOME" )
fi

# Trap all signals that we wish to handle
trap "kill ${!}; exit" TERM INT

run()
{
    eval "\"$JAVA_HOME/bin/java\"  $DEFAULT_JVM_OPTS $JAVA_OPTS $GRADLE_OPTS \" -Dorg.gradle.appname=$APP_BASE_NAME\" -classpath \"$CLASSPATH\" org.gradle.wrapper.GradleWrapperMain \"$@\""
    exit_status=$?
    trap - TERM INT
    wait ${!}
    return $exit_status
}

if [ "$1" = "-v" ] || [ "$1" = "--version" ] ; then
    $JAVA_HOME/bin/java -Dorg.gradle.appname=$APP_BASE_NAME -classpath "$CLASSPATH" org.gradle.wrapper.GradleWrapperMain "-v"
    exit
fi

run "$@"
