Name:           packmule
Version:        0.1.0
Release:        1%{?dist}
Summary:        Air-gapped multi-registry package bundler

License:        MIT
URL:            https://github.com/christopherhagler/packmule
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  cmake
BuildRequires:  make
BuildRequires:  libcurl-devel
BuildRequires:  openssl-devel
BuildRequires:  libarchive-devel
BuildRequires:  cjson-devel

%description
packmule reads a package manifest, queries the appropriate registry to resolve
each entry to a concrete version and download URL, downloads every required
file, and verifies each one against its published SHA-256 or SHA-512 digest.
The result is a self-contained directory that can be transported to a
network-isolated machine and installed without outbound internet access.

Supported registries:
  pypi - Python packages (PyPI); full transitive resolution
  npm  - Node.js packages (npmjs.org); full transitive resolution
  rpm  - RPM packages from any DNF/YUM repository

%prep
%autosetup -n %{name}-%{version}

%build
# Let %%cmake inject the distribution's optimization/hardening flags; do not set
# CMAKE_BUILD_TYPE.  Disable -Werror (compiler bumps would otherwise FTBFS) and
# let RPM's brp-compress handle man-page compression.
%cmake -DPACKMULE_WERROR=OFF -DPACKMULE_COMPRESS_MAN=OFF
%cmake_build

%install
%cmake_install

%check
%ctest

%files
%license LICENSE
%doc README.md
%{_bindir}/packmule
%{_mandir}/man1/packmule.1*

%changelog
* Wed Jun 18 2026 Christopher Hagler <haglerchristopher@gmail.com> - 0.1.0-1
- Initial package
