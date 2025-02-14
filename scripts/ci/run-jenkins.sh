#!/bin/bash -e

TESTCMD=`dirname "${BASH_SOURCE[0]}"`/run-step.sh

export TEST_HARNESS_VERBOSE=1
if [[ ${label} == w* ]]; then
    # Passing -ggdb3 on Cygwin breaks linking against libmonosgen-x.y.dll
    export CFLAGS="-g -O2"
else
    export CFLAGS="-ggdb3 -O2"
fi

if [[ ${CI_TAGS} == *'coop-gc'* ]]; then EXTRA_CONF_FLAGS="${EXTRA_CONF_FLAGS} --with-cooperative-gc=yes"; export MONO_CHECK_MODE=gc,thread; fi

if [[ ${label} == 'osx-i386' ]]; then EXTRA_CONF_FLAGS="${EXTRA_CONF_FLAGS} --with-libgdiplus=/Library/Frameworks/Mono.framework/Versions/Current/lib/libgdiplus.dylib --build=i386-apple-darwin11.2.0"; fi
if [[ ${label} == 'osx-amd64' ]]; then EXTRA_CONF_FLAGS="${EXTRA_CONF_FLAGS} --with-libgdiplus=/Library/Frameworks/Mono.framework/Versions/Current/lib/libgdiplus.dylib "; fi
if [[ ${label} == 'w32' ]]; then PLATFORM=Win32; EXTRA_CONF_FLAGS="${EXTRA_CONF_FLAGS} --host=i686-w64-mingw32"; export MONO_EXECUTABLE="`cygpath -u ${WORKSPACE}\\\msvc\\\build\\\sgen\\\Win32\\\bin\\\Release\\\mono-sgen.exe`"; fi
if [[ ${label} == 'w64' ]]; then PLATFORM=x64; EXTRA_CONF_FLAGS="${EXTRA_CONF_FLAGS} --host=x86_64-w64-mingw32 --disable-boehm"; export MONO_EXECUTABLE="`cygpath -u ${WORKSPACE}\\\msvc\\\build\\\sgen\\\x64\\\bin\\\Release\\\mono-sgen.exe`"; fi

if [[ ${CI_TAGS} == *'mobile_static'* ]];
    then
    EXTRA_CONF_FLAGS="${EXTRA_CONF_FLAGS} --with-runtime_preset=mobile_static";
elif [[ ${CI_TAGS} == *'acceptance-tests'* ]];
    then
    EXTRA_CONF_FLAGS="${EXTRA_CONF_FLAGS} --prefix=${WORKSPACE}/tmp/mono-acceptance-tests --with-sgen-default-concurrent=yes";
elif [[ ${label} != w* ]] && [[ ${label} != 'debian-8-ppc64el' ]] && [[ ${label} != 'centos-s390x' ]] && [[ ${CI_TAGS} != *'monolite'* ]];
    then
    # Override the defaults to skip profiles
    # only enable the mobile profiles and mobile_static on the main architectures
    # only enable the concurrent collector by default on main unix archs
    EXTRA_CONF_FLAGS="${EXTRA_CONF_FLAGS} --with-runtime_preset=all --with-sgen-default-concurrent=yes"
fi

if [ -x "/usr/bin/dpkg-architecture" ];
	then
	EXTRA_CONF_FLAGS="$EXTRA_CONF_FLAGS --host=`/usr/bin/dpkg-architecture -qDEB_HOST_GNU_TYPE`"
	#force build arch = dpkg arch, sometimes misdetected
	mkdir -p ~/.config/.mono/
	wget -qO- https://download.mono-project.com/test/new-certs.tgz| tar zx -C ~/.config/.mono/
fi


${TESTCMD} --label=configure --timeout=60m --fatal ./autogen.sh $EXTRA_CONF_FLAGS
if [[ ${label} == 'w32' ]];
    then
	# only build boehm on w32 (only windows platform supporting boehm).
    ${TESTCMD} --label=make-msvc --timeout=60m --fatal /cygdrive/c/Program\ Files\ \(x86\)/MSBuild/14.0/Bin/MSBuild.exe /p:PlatformToolset=v140 /p:Platform=${PLATFORM} /p:Configuration=Release /p:MONO_TARGET_GC=boehm msvc/mono.sln
fi
if [[ ${label} == w* ]];
    then
    ${TESTCMD} --label=make-msvc-sgen --timeout=60m --fatal /cygdrive/c/Program\ Files\ \(x86\)/MSBuild/14.0/Bin/MSBuild.exe /p:PlatformToolset=v140 /p:Platform=${PLATFORM} /p:Configuration=Release /p:MONO_TARGET_GC=sgen msvc/mono.sln
fi

if [[ ${CI_TAGS} == *'monolite'* ]]; then make get-monolite-latest; fi

make_parallelism=-j4
if [[ ${label} == 'debian-8-ppc64el' ]]; then make_parallelism=-j1; fi

${TESTCMD} --label=make --timeout=300m --fatal make ${make_parallelism} -w V=1
if [[ -n "${ghprbPullId}" ]] && [[ ${label} == w* ]];
    then
    exit 0
    # we don't run the test suite on Windows PRs, we just ensure the build succeeds, so end here
fi

if [[ ${CI_TAGS} == *'acceptance-tests'* ]];
    then
	$(dirname "${BASH_SOURCE[0]}")/run-test-acceptance-tests.sh
elif [[ ${CI_TAGS} == *'profiler-stress-tests'* ]];
    then
	$(dirname "${BASH_SOURCE[0]}")/run-test-profiler-stress-tests.sh
elif [[ ${CI_TAGS} == *'no-tests'* ]];
    then
	exit 0
else
	make check-ci
fi
