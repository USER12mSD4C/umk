Name:           umk
Version:        1.0.0
Release:        %bcond_with check1%{?dist}
Summary:        Simple build system

License:        MIT
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
Requires:       /bin/sh

%description
umk is a simple build system. It reads a UMK file in the current
directory. It supports variables, pattern rules, commands, flags,
content hash caching, and parallel jobs.

%prep
%setup -q

%build
%set_build_flags
%{__cc} %{optflags} -std=c11 -o umk umk.c

%install
install -Dpm 0755 umk %{buildroot}%{_bindir}/umk
install -Dpm 0644 umk.1 %{buildroot}%{_mandir}/man1/umk.1

%check
%if %{with check}
sh tests/run.sh ./umk
%endif

%files
%license LICENSE
%doc README.md
%{_bindir}/umk
%{_mandir}/man1/umk.1*

%changelog
* Sun Jun 15 2026 user12ms - 1.0.0-1
- Initial package.
