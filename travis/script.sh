#!/bin/bash
#
# $Id$
#
# Copyright 2016-2017 Quantcast Corporation. All rights reserved.
#
# This file is part of Quantcast File System.
#
# Licensed under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing
# permissions and limitations under the License.

################################################################################
# The following is executed on .travis.yml's script section
################################################################################

set -ex

DEPS_UBUNTU="g++ cmake git libboost-regex-dev libkrb5-dev libssl-dev python-dev libfuse-dev default-jdk wget unzip maven sudo"
DEPS_CENTOS="gcc-c++ make cmake git boost-devel krb5-devel openssl-devel python-devel fuse-devel java-openjdk java-devel libuuid-devel wget unzip sudo which"

MVN_TAR="apache-maven-3.0.5-bin.tar.gz"
MVN_URL="http://mirror.cc.columbia.edu/pub/software/apache/maven/maven-3/3.0.5/binaries/$MVN_TAR"
TAIL_TEST_LOGS='{ find build/release/qfstest -type f -name \*.log -print0 | xargs -0  tail -n 500 ; exit 1; }'

MYCMAKE_OPTIONS='-D CMAKE_BUILD_TYPE=RelWithDebInfo'

if [[ "$TRAVIS_OS_NAME" == "osx" ]]; then
    MYOPENSSL_DIR='/usr/local/Cellar/openssl/'
    MYOPENSSL_R_DIR=$(ls -1 "$MYOPENSSL_DIR" | tail -n 1)
    MYCMAKE_OPTIONS="$MYCMAKE_OPTIONS -D OPENSSL_ROOT_DIR=${MYOPENSSL_DIR}${MYOPENSSL_R_DIR}"
    sysctl machdep.cpu
    df -h
    make -j 2 CMAKE_OPTIONS="$MYCMAKE_OPTIONS" test tarball || $TAIL_TEST_LOGS
fi

if [[ "$TRAVIS_OS_NAME" == "linux" ]]; then
    MYCMAKE_OPTIONS="$MYCMAKE_OPTIONS -D QFS_EXTRA_CXX_OPTIONS=-Werror"
    CMD='if [ x"$(id -u)" = x0 ]; then MYSUDO=; else MYSUDO=sudo; fi; '
    if [[ "$DISTRO" == "ubuntu" ]]; then
        CMD="$CMD $MYSUDO apt-get update"
        CMD="$CMD && $MYSUDO apt-get install -y $DEPS_UBUNTU"

        # coverage enabled only generated on ubuntu
        MYCMAKE_OPTIONS="$MYCMAKE_OPTIONS -D ENABLE_COVERAGE=ON"
    elif [[ "$DISTRO" == "centos" ]]; then
        CMD="yum install -y $DEPS_CENTOS"

        # CentOS doesn't package maven directly so we have to install it manually
        CMD="$CMD && wget $MVN_URL"
        CMD="$CMD && $MYSUDO tar -xf $MVN_TAR -C /usr/local"

        # Set up PATH and links
        CMD="$CMD && pushd /usr/local"
        CMD="$CMD && $MYSUDO ln -s ${MVN_TAR%-bin.tar.gz} maven"
        CMD="$CMD && export M2_HOME=/usr/local/maven"
        CMD="$CMD && export PATH=\${M2_HOME}/bin:\${PATH}"
        CMD="$CMD && popd"

	if [[ "$VER" == "7" ]]; then
	    # CentOS7 has the distro information in /etc/redhat-release
	    CMD="$CMD && cut /etc/redhat-release -d' ' --fields=1,3,4 > /etc/issue"
	fi
    fi
    CMD="$CMD && { cat /proc/cpuinfo ; df -h ; true ; }"
    CMD="$CMD && make -j 2 CMAKE_OPTIONS='$MYCMAKE_OPTIONS' rat test tarball"
    CMD="$CMD || $TAIL_TEST_LOGS"

    docker run --rm -t -v $PWD:$PWD -w $PWD $DISTRO:$VER /bin/bash -c "$CMD"
fi

# vim: set tw=0: