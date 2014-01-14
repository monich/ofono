# 
# Do NOT Edit the Auto-generated Part!
# Generated by: spectacle version 0.26
# 

Name:       ofono

# >> macros
# << macros

Summary:    Open Source Telephony
Version:    1.14
Release:    1
Group:      Communications/Connectivity Adaptation
License:    GPLv2
URL:        http://ofono.org
Source0:    http://www.kernel.org/pub/linux/network/ofono/ofono-%{version}.tar.xz
Source100:  ofono.yaml
Requires:   dbus
Requires:   systemd
Requires:   ofono-configs
Requires(preun): systemd
Requires(post): systemd
Requires(postun): systemd
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(libudev) >= 145
BuildRequires:  pkgconfig(bluez) >= 4.85
BuildRequires:  pkgconfig(mobile-broadband-provider-info)
BuildRequires:  libtool
BuildRequires:  automake
BuildRequires:  autoconf

%description
Telephony stack

%package devel
Summary:    Headers for oFono
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel
Development headers and libraries for oFono

%package tests
Summary:    Test Scripts for oFono
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}
Requires:   dbus-python
Requires:   pygobject2
Provides:   ofono-test >= 1.0
Obsoletes:  ofono-test < 1.0

%description tests
Scripts for testing oFono and its functionality

%package configs-mer
Summary:    Package to provide default configs for ofono
Group:      Development/Tools
Requires:   %{name} = %{version}-%{release}
Provides:   ofono-configs

%description configs-mer
This package provides default configs for ofono

%prep
%setup -q -n %{name}-%{version}/%{name}

# >> setup
./bootstrap
# << setup

%build
# >> build pre
autoreconf --force --install
# << build pre

%configure --disable-static \
    --enable-dundee \
    --enable-test \
    --with-systemdunitdir="/%{_lib}/systemd/system"

make %{?jobs:-j%jobs}

# >> build post
# << build post

%install
rm -rf %{buildroot}
# >> install pre
# << install pre
%make_install

# >> install post
mkdir -p %{buildroot}/%{_lib}/systemd/system/network.target.wants
ln -s ../ofono.service %{buildroot}/%{_lib}/systemd/system/network.target.wants/ofono.service
# << install post

%preun
if [ "$1" -eq 0 ]; then
systemctl stop ofono.service
fi

%post
systemctl daemon-reload
systemctl reload-or-try-restart ofono.service

%postun
systemctl daemon-reload

%files
%defattr(-,root,root,-)
# >> files
%doc COPYING ChangeLog AUTHORS README
%config(noreplace) %{_sysconfdir}/dbus-1/system.d/*.conf
%{_sbindir}/*
/%{_lib}/systemd/system/network.target.wants/ofono.service
/%{_lib}/systemd/system/ofono.service
/%{_lib}/systemd/system/dundee.service
%dir %{_sysconfdir}/ofono/
# This file is part of phonesim and not needed with ofono.
%exclude %{_sysconfdir}/ofono/phonesim.conf
%doc /usr/share/man/man8/ofonod.8.gz
%dir %attr(775,radio,radio) /var/lib/ofono
# << files

%files devel
%defattr(-,root,root,-)
%{_includedir}/ofono/
%{_libdir}/pkgconfig/ofono.pc
# >> files devel
# << files devel

%files tests
%defattr(-,root,root,-)
%{_libdir}/%{name}/test/*
# >> files tests
# << files tests

%files configs-mer
%defattr(-,root,root,-)
%config /etc/ofono/ril_subscription.conf
# >> files ofono-configs-mer
# << files ofono-configs-mer
