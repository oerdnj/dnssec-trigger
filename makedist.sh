# makedist.sh: makes distributable tarball.

# Abort script on unexpected errors.
set -e

# Remember the current working directory.
cwd=`pwd`

# Utility functions.
usage () {
    cat >&2 <<EOF
Usage $0: [-h] [-s] [-d SVN_root] [-l ldns_path] [-w ...args...]
Generate a distribution tar file for dnssec-trigger.

    -h           This usage information.
    -s           Build a snapshot distribution file.  The current date is
                 automatically appended to the current version number.
    -rc <nr>     Build a release candidate, the given string will be added
                 to the version number 
                 (which will then be dnssec-trigger-<version>rc<number>)
    -d SVN_root  Retrieve the source from the specified repository.
                 Detected from svn working copy if not specified.
    -l ldnsdir   Directory where ldns resides. Detected from Makefile.
    -wssl openssl.xx.tar.gz Also build openssl from tarball for windows dist.
    -wldns ldns.xx.tar.gz Also build libldns from tarball for windows dist.
    -w ...       Build windows binary dist. last args passed to configure.
EOF
    exit 1
}

info () {
    echo "$0: info: $1"
}

error () {
    echo "$0: error: $1" >&2
    exit 1
}

question () {
    printf "%s (y/n) " "$*"
    read answer
    case "$answer" in
        [Yy]|[Yy][Ee][Ss])
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

replace_text () {
    (cp "$1" "$1".orig && \
        sed -e "s/$2/$3/g" < "$1".orig > "$1" && \
        rm "$1".orig) || error_cleanup "Replacement for $1 failed."
}

# Only use cleanup and error_cleanup after generating the temporary
# working directory.
cleanup () {
    info "Deleting temporary working directory."
    cd $cwd && rm -rf $temp_dir
}

error_cleanup () {
    echo "$0: error: $1" >&2
    cleanup
    exit 1
}

check_svn_root () {
    # Check if SVNROOT is specified.
    if [ -z "$SVNROOT" ]; then
        if test -f .svn/entries; then
              eval `svn info | grep 'URL:' | sed -e 's/URL: /url=/' | head -1`
              SVNROOT="$url"
        fi
        if test -z "$SVNROOT"; then
            error "SVNROOT must be specified (using -d)"
        fi
    fi
}

create_temp_dir () {
    # Creating temp directory
    info "Creating temporary working directory"
    temp_dir=`mktemp -d makedist-XXXXXX`
    info "Directory '$temp_dir' created."
    cd $temp_dir
}


SNAPSHOT="no"
RC="no"
LDNSDIR=""
DOWIN="no"
WINSSL=""
WINLDNS=""

# Parse the command line arguments.
while [ "$1" ]; do
    case "$1" in
        "-h")
            usage
            ;;
        "-d")
            SVNROOT="$2"
            shift
            ;;
        "-s")
            SNAPSHOT="yes"
            ;;
        "-wldns")
            WINLDNS="$2"
            shift
            ;;
        "-wssl")
            WINSSL="$2"
            shift
            ;;
        "-w")
            DOWIN="yes"
            shift
            break
            ;;
        "-l")
            LDNSDIR="$2"
            shift
            ;;
        "-rc")
            RC="$2"
            shift
            ;;
        *)
            error "Unrecognized argument -- $1"
            ;;
    esac
    shift
done

if [ "$DOWIN" = "yes" ]; then
    # detect crosscompile, from Fedora13 at this point.
    if test "`uname`" = "Linux"; then
        info "Crosscompile windows dist"
        cross="yes"
        configure="mingw32-configure"
        strip="i686-pc-mingw32-strip"
        makensis="makensis"     # from mingw32-nsis package
        # flags for crosscompiled dependency libraries
        cross_flag=""

        check_svn_root
        create_temp_dir

        # crosscompile openssl for windows.
        if test -n "$WINSSL"; then
                info "Cross compile $WINSSL"
                info "winssl tar unpack"
                (cd ..; gzip -cd $WINSSL) | tar xf - || error_cleanup "tar unpack of $WINSSL failed"
                sslinstall="`pwd`/sslinstall"
                cd openssl-* || error_cleanup "no openssl-X dir in tarball"
                # configure for crosscompile, without CAPI because it fails
                # cross-compilation and it is not used anyway
                sslflags="no-asm --cross-compile-prefix=i686-pc-mingw32- -DOPENSSL_NO_CAPIENG mingw"
                info "winssl: Configure $sslflags"
                ./Configure --prefix="$sslinstall" $sslflags || error_cleanup "OpenSSL Configure failed"
                info "winssl: make"
                make || error_cleanup "OpenSSL crosscompile failed"
                # only install sw not docs, which take a long time.
                info "winssl: make install_sw"
                make install_sw || error_cleanup "OpenSSL install failed"
                cross_flag="$cross_flag --with-ssl=$sslinstall"
                cd ..
        fi

        if test -n "$WINLDNS"; then
                info "Cross compile $WINLDNS"
                info "ldns tar unpack"
                (cd ..; gzip -cd $WINLDNS) | tar xf - || error_cleanup "tar unpack of $WINLDNS failed"
                cd ldns-* || error_cleanup "no ldns-X dir in tarball"
                # we can use the cross_flag with openssl in it
                info "ldns: Configure $cross_flag"
                mingw32-configure  $cross_flag || error_cleanup "ldns configure failed"
                info "ldns: make"
                make || error_cleanup "ldns crosscompile failed"
                # use from the build directory.
                cross_flag="$cross_flag --with-ldns=`pwd`"
                cd ..
        fi


        info "Exporting source from SVN."
        svn export "$SVNROOT" dnssec-trigger || error_cleanup "SVN command failed"
        cd dnssec-trigger || error_cleanup "Not exported correctly from SVN"

        # on a re-configure the cache may no longer be valid...
        if test -f mingw32-config.cache; then rm mingw32-config.cache; fi
    else
        cross="no"      # mingw and msys
        cross_flag=""
        configure="./configure"
        strip="strip"
        makensis="c:/Program Files/NSIS/makensis.exe" # http://nsis.sf.net
    fi

    # version gets compiled into source, edit the configure to set  it
    version=`./configure --version | head -1 | awk '{ print $3 }'` \
        || error_cleanup "Cannot determine version number."
    if [ "$RC" != "no" -o "$SNAPSHOT" != "no" ]; then
        if [ "$RC" != "no" ]; then
                version2=`echo $version | sed -e 's/rc.*$//' -e 's/_20.*$//'`
                version2=`echo $version2 | sed -e 's/rc.*//'`"rc$RC"
        fi
        if [ "$SNAPSHOT" != "no" ]; then
                version2=`echo $version | sed -e 's/rc.*$//' -e 's/_20.*$//'`
                version2="${version2}_`date +%Y%m%d`"
        fi
        replace_text "configure.ac" "AC_INIT(dnssec-trigger, $version" "AC_INIT(dnssec-trigger, $version2"
        version="$version2"
        info "Rebuilding configure script (autoconf) snapshot."
        autoconf || error_cleanup "Autoconf failed."
        autoheader || error_cleanup "Autoheader failed."
        rm -r autom4te* || echo "ignored"
    fi

    # procedure for making installer on mingw. 
    info "Creating windows dist dnssec-trigger $version"
    info "Calling configure"
    echo "$configure"' --enable-debug --enable-static-exe '"$* $cross_flag"
    $configure --enable-debug --enable-static-exe $* $cross_flag \
        || error_cleanup "Could not configure"
    info "Calling make"
    make || error_cleanup "Could not make"
    info "Make complete"

    info "dnssec-trigger version: $version"
    file="dnssec-trigger-$version.zip"
    rm -f $file
    info "Creating $file"
    mkdir tmp.$$
    make strip || error_exit "could not strip"
    cd tmp.$$
    # TODO files and crosscompile
    # DLLs linked with the panel on windows (ship DLLs:)
    # libgdk-win32-2.0-0.dll  -> /opt/gtk/bin/libgdk-win32-2.0-0.dll
    # libgdk_pixbuf-2.0-0.dll -> /opt/gtk/bin/libgdk_pixbuf-2.0-0.dll
    # libglib-2.0-0.dll       -> /opt/gtk/bin/libglib-2.0-0.dll
    # libgobject-2.0-0.dll    -> /opt/gtk/bin/libgobject-2.0-0.dll
    # libgthread-2.0-0.dll    -> /opt/gtk/bin/libgthread-2.0-0.dll
    # libgtk-win32-2.0-0.dll  -> /opt/gtk/bin/libgtk-win32-2.0-0.dll
    # libatk-1.0-0.dll        -> /opt/gtk/bin/libatk-1.0-0.dll
    # libcairo-2.dll  -> /opt/gtk/bin/libcairo-2.dll
    # intl.dll        -> /opt/gtk/bin/intl.dll
    # libgio-2.0-0.dll        -> /opt/gtk/bin/libgio-2.0-0.dll
    # libgmodule-2.0-0.dll    -> /opt/gtk/bin/libgmodule-2.0-0.dll
    # libpango-1.0-0.dll      -> /opt/gtk/bin/libpango-1.0-0.dll
    # libpangocairo-1.0-0.dll -> /opt/gtk/bin/libpangocairo-1.0-0.dll
    # libpangowin32-1.0-0.dll -> /opt/gtk/bin/libpangowin32-1.0-0.dll
    # libpng14-14.dll -> /opt/gtk/bin/libpng14-14.dll
    # zlib1.dll       -> /opt/gtk/bin/zlib1.dll
    # libpangoft2-1.0-0.dll   -> /opt/gtk/bin/libpangoft2-1.0-0.dll
    # libfontconfig-1.dll     -> /opt/gtk/bin/libfontconfig-1.dll
    # freetype6.dll   -> /opt/gtk/bin/freetype6.dll
    # libexpat-1.dll  -> /usr/local/bin/libexpat-1.dll


    cp ../example.conf example.conf
    cp ../dnssec-triggerd.exe ../dnssec-trigger-control.exe ../dnssec-trigger-panel.exe .
    # zipfile
    zip ../$file example.conf dnssec-triggerd.exe dnssec-trigger-control.exe dnssec-trigger-panel.exe
    info "Testing $file"
    (cd .. ; zip -T $file )
    # installer
    info "Creating installer"
    quadversion=`cat ../config.h | grep RSRC_PACKAGE_VERSION | sed -e 's/#define RSRC_PACKAGE_VERSION //' -e 's/,/\\./g'`
    cat ../winrc/setup.nsi | sed -e 's/define VERSION.*$/define VERSION "'$version'"/' -e 's/define QUADVERSION.*$/define QUADVERSION "'$quadversion'"/' > ../winrc/setup_ed.nsi
    "$makensis" ../winrc/setup_ed.nsi
    info "Created installer"
    cd ..
    rm -rf tmp.$$
    mv winrc/dnssec-trigger-setup-$version.exe .
    if test "$cross" = "yes"; then
            mv dnssec-trigger-setup-$version.exe $cwd/.
            mv $file $cwd/.
            cleanup
    fi
    ls -lG dnssec-trigger-setup-$version.exe
    ls -lG $file
    info "Done"
    exit 0
fi

check_svn_root

# Start the packaging process.
info "SVNROOT  is $SVNROOT"
info "SNAPSHOT is $SNAPSHOT"

create_temp_dir

info "Exporting source from SVN."
svn export "$SVNROOT" dnssec-trigger || error_cleanup "SVN command failed"

cd dnssec-trigger || error_cleanup "Not exported correctly from SVN"

find . -name .c-mode-rc.el -exec rm {} \;
find . -name .cvsignore -exec rm {} \;
rm makedist.sh || error_cleanup "Failed to remove makedist.sh."

info "Determining version."
version=`./configure --version | head -1 | awk '{ print $3 }'` || \
    error_cleanup "Cannot determine version number."

info "version: $version"

RECONFIGURE="no"

if [ "$RC" != "no" ]; then
    info "Building release candidate $RC."
    version2="${version}rc$RC"
    info "Version number: $version2"

    replace_text "configure.ac" "AC_INIT(dnssec-trigger, $version" "AC_INIT(dnssec-trigger, $version2"
    version="$version2"
    RECONFIGURE="yes"
fi

if [ "$SNAPSHOT" = "yes" ]; then
    info "Building snapshot."
    version2="${version}_`date +%Y%m%d`"
    info "Snapshot version number: $version2"

    replace_text "configure.ac" "AC_INIT(dnssec-trigger, $version" "AC_INIT(dnssec-trigger, $version2"
    version="$version2"
    RECONFIGURE="yes"
fi

if [ "$RECONFIGURE" = "yes" ]; then
    info "Rebuilding configure script (autoconf) snapshot."
    autoconf || error_cleanup "Autoconf failed."
    autoheader || error_cleanup "Autoheader failed."
    rm -r autom4te* || error_cleanup "Failed to remove autoconf cache directory."
fi

info "Renaming directory to dnssec-trigger-$version."
cd ..
mv dnssec-trigger dnssec-trigger-$version || error_cleanup "Failed to rename directory."

tarfile="dnssec-trigger-$version.tar.gz"

if [ -f ../$tarfile ]; then
    (question "The file ../$tarfile already exists.  Overwrite?" \
        && rm -f ../$tarfile) || error_cleanup "User abort."
fi

info "Creating tar dnssec-trigger-$version.tar.gz"
tar czf ../$tarfile dnssec-trigger-$version || error_cleanup "Failed to create tar file."

cleanup

case `uname 2>&1` in
    Linux|linux) 
        sha=`sha1sum $tarfile |  awk '{ print $1 }'`
        sha256=`sha256sum $tarfile |  awk '{ print $1 }'`
    ;;
    FreeBSD|freebsd)
        sha=`sha1 $tarfile |  awk '{ print $5 }'`
        sha256=`sha256 $tarfile |  awk '{ print $5 }'`
    ;;
    *)
        sha=`sha1sum $tarfile |  awk '{ print $1 }'`
        sha256=`sha256sum $tarfile |  awk '{ print $1 }'`
    ;;
esac

echo $sha > $tarfile.sha1
echo $sha256 > $tarfile.sha256

info "dnssec-trigger distribution created successfully."
info "sha1   $sha"
info "sha256 $sha256"

