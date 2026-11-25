fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    if let Err(err) = pkg_config::Config::new().probe("libnftables") {
        panic!("libnftables development library is required by multiwifi-core: {err}");
    }
}
