//! Compatibility admission for Hyprland background input.
//! This is not a security boundary against same-user code. Unknown package
//! versions refuse until the native operation matrix is qualified for them.
//! Users may admit further executables they have qualified themselves.

use std::io::Read;
use std::path::{Path, PathBuf};

struct QualifiedPackage {
    name: &'static str,
    version: &'static str,
    executable: &'static str,
}

const PACKAGES: &[QualifiedPackage] = &[
    QualifiedPackage {
        name: "libreoffice-fresh",
        version: "26.2.5-3",
        executable: "/usr/lib/libreoffice/program/soffice.bin",
    },
    QualifiedPackage {
        name: "inkscape",
        version: "1.4.4-6",
        executable: "/usr/bin/inkscape",
    },
];

/// Who vouched for the app. Results for a user entry must say so, because Cua
/// has not run its operation matrix against that app.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Qualification {
    Package,
    User,
}

impl Qualification {
    pub(super) fn label(self) -> &'static str {
        match self {
            Self::Package => "package",
            Self::User => "user",
        }
    }
}

fn field<'a>(description: &'a str, name: &str) -> Option<&'a str> {
    let mut lines = description.lines();
    while let Some(line) = lines.next() {
        if line == name {
            return lines.next().filter(|value| !value.is_empty());
        }
    }
    None
}

fn read_bounded(path: &Path) -> Result<String, &'static str> {
    let file = std::fs::File::open(path).map_err(|_| "client_qualification_unavailable")?;
    let mut content = String::new();
    file.take(1024 * 1024 + 1)
        .read_to_string(&mut content)
        .map_err(|_| "client_qualification_unavailable")?;
    if content.len() > 1024 * 1024 {
        return Err("client_qualification_unavailable");
    }
    Ok(content)
}

fn user_list_path() -> Option<PathBuf> {
    Some(
        dirs::config_dir()?
            .join("cua-driver")
            .join("hyprland-apps.txt"),
    )
}

/// One absolute executable path per line. Blank lines and `#` comments are
/// ignored. Entries are canonicalized so a symlink names its real binary.
fn user_listed(list: &str, executable: &Path) -> bool {
    list.lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .filter(|line| Path::new(line).is_absolute())
        .any(|line| std::fs::canonicalize(line).ok().as_deref() == Some(executable))
}

fn package_qualified(executable: &Path) -> Result<bool, &'static str> {
    let Some(package) = PACKAGES.iter().find(|package| {
        std::fs::canonicalize(package.executable).ok().as_deref() == Some(executable)
    }) else {
        return Ok(false);
    };
    let directory = PathBuf::from("/var/lib/pacman/local")
        .join(format!("{}-{}", package.name, package.version));
    let description = read_bounded(&directory.join("desc"))?;
    if field(&description, "%NAME%") != Some(package.name)
        || field(&description, "%VERSION%") != Some(package.version)
    {
        return Ok(false);
    }
    let files = read_bounded(&directory.join("files"))?;
    Ok(files
        .lines()
        .any(|line| line == package.executable.trim_start_matches('/')))
}

/// Cua's package matrix covers native windows only, so an X window of the
/// same app still needs the user's entry.
pub(super) fn qualify(pid: u32, xwayland: bool) -> Result<Qualification, &'static str> {
    let process_exe = PathBuf::from(format!("/proc/{pid}/exe"));
    let executable =
        std::fs::read_link(&process_exe).map_err(|_| "client_qualification_unavailable")?;
    let qualification = if !xwayland && package_qualified(&executable)? {
        Qualification::Package
    } else if user_list_path()
        .and_then(|path| read_bounded(&path).ok())
        .is_some_and(|list| user_listed(&list, &executable))
    {
        Qualification::User
    } else {
        return Err("client_not_qualified");
    };
    // Detect replacement between initial identification and admission. The
    // compositor independently verifies its exact live target at dispatch.
    if std::fs::read_link(process_exe).ok().as_ref() != Some(&executable) {
        return Err("client_qualification_unavailable");
    }
    Ok(qualification)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alpm_identity_requires_exact_package_and_version_fields() {
        let description = "%NAME%\ninkscape\n\n%VERSION%\n1.4.4-6\n\n";
        assert_eq!(field(description, "%NAME%"), Some("inkscape"));
        assert_eq!(field(description, "%VERSION%"), Some("1.4.4-6"));
        assert_eq!(field("%VERSION%\n", "%VERSION%"), None);
        assert_eq!(field("%VERSION%\n\n", "%VERSION%"), None);
        assert_eq!(field("prefix%NAME%\ninkscape", "%NAME%"), None);
    }

    #[test]
    fn nonexistent_process_cannot_be_qualified() {
        assert_eq!(
            qualify(u32::MAX, false),
            Err("client_qualification_unavailable")
        );
    }

    #[test]
    fn user_list_admits_only_exact_absolute_executables() {
        let home = tempfile::tempdir().unwrap();
        let binary = home.path().join("editor");
        std::fs::write(&binary, b"").unwrap();
        let link = home.path().join("editor-link");
        std::os::unix::fs::symlink(&binary, &link).unwrap();
        let canonical = std::fs::canonicalize(&binary).unwrap();
        let listed = |list: &str| user_listed(list, &canonical);

        assert!(listed(&format!("# mine\n\n  {}  \n", binary.display())));
        assert!(listed(&link.display().to_string()));
        assert!(!listed(&format!("# {}", binary.display())));
        assert!(!listed("editor"));
        assert!(!listed(&format!("{}/other", home.path().display())));
        assert!(!listed(""));
    }
}
