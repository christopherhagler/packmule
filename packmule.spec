Name:           packmule
Version:        0.1.0
Release:        1%{?dist}
Summary:        Air-gapped multi-registry package bundler

License:        MIT
URL:            https://github.com/christopherhagler/packmule
Source0:        https://github.com/christopherhagler/packmule/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.15
BuildRequires:  gcc
BuildRequires:  libcurl-devel
BuildRequires:  openssl-devel
BuildRequires:  libarchive-devel
BuildRequires:  cjson-devel
BuildRequires:  gzip

%description
packmule reads a package manifest, queries the appropriate registry to resolve
each entry to a concrete version and download URL, downloads every required
file, and verifies each one against its published SHA-256 or SHA-512 digest.
The result is a self-contained directory that can be transported to a
network-isolated machine and installed without outbound internet access.

Supported registries:
  pypi  — Python packages (PyPI); full transitive resolution
  npm   — Node.js packages (npmjs.org); full transitive resolution
  rpm   — RPM packages from any DNF/YUM repository

%prep
%autosetup

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%check
cd %{_vpath_builddir}
ctest --output-on-failure

%files
%license LICENSE
%doc README.md
%{_bindir}/packmule
%{_mandir}/man1/packmule.1*

%changelog
* Sun Jun 07 2026 Christopher Hagler <haglerchristopher@gmail.com> - 0.1.0-1
- Initial package
