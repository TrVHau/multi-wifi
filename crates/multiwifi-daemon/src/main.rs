#![deny(unsafe_code)]

use multiwifi_daemon::polkit::BusKind;
use multiwifi_daemon::service::{DaemonServer, ServiceConfig, DAEMON_API_VERSION, DAEMON_VERSION};
use std::env;
use std::process::ExitCode;
use tokio::signal::unix::{signal, SignalKind};

enum CliAction {
    Help,
    Version,
    Run {
        bus_kind: BusKind,
        enable_polkit: bool,
    },
}

fn parse_args() -> Result<CliAction, String> {
    let mut bus_kind = BusKind::System;
    let mut enable_polkit = true;

    for arg in env::args().skip(1) {
        match arg.as_str() {
            "--session" | "-s" => bus_kind = BusKind::Session,
            "--no-polkit" => enable_polkit = false,
            "--version" | "-v" => return Ok(CliAction::Version),
            "--help" | "-h" => return Ok(CliAction::Help),
            other => return Err(format!("Unknown argument: {other}")),
        }
    }
    Ok(CliAction::Run {
        bus_kind,
        enable_polkit,
    })
}

#[tokio::main]
async fn main() -> ExitCode {
    let (bus_kind, enable_polkit) = match parse_args() {
        Ok(CliAction::Help) => {
            println!(
                "Usage:\n  multiwifi-daemon [OPTION?]\n\n\
                 Options:\n  \
                 -h, --help         Show help options\n  \
                 -s, --session      Run on session bus instead of system bus (for testing)\n  \
                 --no-polkit        Disable polkit authorization enforcement (for testing)\n  \
                 -v, --version      Show daemon version and exit"
            );
            return ExitCode::SUCCESS;
        }
        Ok(CliAction::Version) => {
            println!("multiwifi-daemon {DAEMON_VERSION} (API {DAEMON_API_VERSION})");
            return ExitCode::SUCCESS;
        }
        Ok(CliAction::Run {
            bus_kind,
            enable_polkit,
        }) => (bus_kind, enable_polkit),
        Err(err) => {
            eprintln!("Failed to parse arguments: {err}");
            return ExitCode::FAILURE;
        }
    };

    let use_session_bus = bus_kind == BusKind::Session;

    eprintln!(
        "Starting MultiWiFi Rust System Daemon v{DAEMON_VERSION} (API {DAEMON_API_VERSION}, D-Bus: {}, Polkit: {})",
        if use_session_bus { "SESSION BUS" } else { "SYSTEM BUS" },
        if enable_polkit { "ENABLED" } else { "DISABLED" }
    );

    let config = ServiceConfig::production(bus_kind, enable_polkit);
    let server = match DaemonServer::start(config).await {
        Ok(s) => s,
        Err(err) => {
            eprintln!("Failed to start D-Bus service: {err}");
            return ExitCode::FAILURE;
        }
    };

    let mut shutdown_rx = server.core.subscribe_shutdown();
    let Ok(mut sigint) = signal(SignalKind::interrupt()) else {
        eprintln!("Failed to register SIGINT handler");
        server.stop().await;
        return ExitCode::FAILURE;
    };
    let Ok(mut sigterm) = signal(SignalKind::terminate()) else {
        eprintln!("Failed to register SIGTERM handler");
        server.stop().await;
        return ExitCode::FAILURE;
    };

    tokio::select! {
        _ = sigint.recv() => {
            eprintln!("Received SIGINT, shutting down MultiWiFi System Daemon...");
        }
        _ = sigterm.recv() => {
            eprintln!("Received SIGTERM, shutting down MultiWiFi System Daemon...");
        }
        res = shutdown_rx.changed() => {
            if res.is_ok() && *shutdown_rx.borrow() {
                eprintln!("StopDaemon requested via D-Bus, shutting down cleanly...");
            }
        }
    }

    server.stop().await;
    eprintln!("MultiWiFi System Daemon stopped cleanly.");
    ExitCode::SUCCESS
}
