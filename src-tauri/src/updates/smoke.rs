//! Debug-only end-to-end updater harness. Installation is confined to the
//! explicitly provisioned Windows Sandbox; there is no production test switch.
use super::{
    model::{UpdateOperation, UpdateStatus},
    service::{UpdateGate, UpdateService},
};
use serde_json::{json, Value};
use std::{
    path::Path,
    sync::{Arc, Mutex, OnceLock},
    time::Duration,
};
use tauri::{Manager, RunEvent};

static CERTIFICATE: OnceLock<reqwest::Certificate> = OnceLock::new();
const INPUT: &str = r"C:\CliptureInput";
const OUTPUT: &str = r"C:\CliptureOutput\updater-native.json";
const INSTALLED: &str = r"C:\CliptureInstallerTest\Update Candidate\clipture.exe";

pub fn configure_client(builder: reqwest::ClientBuilder) -> reqwest::ClientBuilder {
    match CERTIFICATE.get() {
        Some(certificate) => builder.add_root_certificate(certificate.clone()).no_proxy(),
        None => builder,
    }
}

pub fn run_if_requested() -> bool {
    if !std::env::args().any(|arg| arg == "--updater-smoke") {
        return false;
    }
    if let Err(error) = run() {
        eprintln!("Isolated updater test failed: {error}");
        std::process::exit(1);
    }
    true
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let token = std::env::var("CLIPTURE_UPDATER_SMOKE_TOKEN")?;
    if std::env::var("USERNAME").as_deref() != Ok("WDAGUtilityAccount")
        || token.len() != 36
        || std::fs::read_to_string(Path::new(INPUT).join("probe-token.txt"))?.trim() != token
        || !std::env::current_exe()?
            .to_string_lossy()
            .eq_ignore_ascii_case(INSTALLED)
    {
        return Err("Refusing updater installation outside the dedicated Sandbox fixture".into());
    }
    let bytes = std::fs::read(Path::new(INPUT).join("updater-fixture.json"))?;
    if bytes.len() > 16 * 1024 {
        return Err("Oversized updater fixture".into());
    }
    let fixture: Value = serde_json::from_slice(&bytes)?;
    let endpoint = fixture["endpoint"]
        .as_str()
        .ok_or("Missing fixture endpoint")?;
    let url = reqwest::Url::parse(endpoint)?;
    if url.scheme() != "https" || url.host_str() != Some("localhost") || url.port() != Some(18443) {
        return Err("Updater fixture must use the dedicated loopback HTTPS server".into());
    }
    let certificate =
        reqwest::Certificate::from_pem(&std::fs::read(Path::new(INPUT).join("updater-cert.pem"))?)?;
    CERTIFICATE
        .set(certificate)
        .map_err(|_| "Updater fixture already initialized")?;
    let mut context = tauri::generate_context!();
    context.config_mut().plugins.0.insert(
        "updater".into(),
        json!({
            "pubkey": fixture["pubkey"], "endpoints": [endpoint],
            "windows": {"installMode": "quiet"}
        }),
    );
    let report = Arc::new(Report(Mutex::new(
        json!({"ok":false,"checks":[],"token":token}),
    )));
    report.record("guarded fixture initialized; TLS verification remains enabled")?;
    let gate = Arc::new(SmokeGate(report.clone()));
    let service = Arc::new(UpdateService::new(gate));
    let application = tauri::Builder::default()
        .plugin(tauri_plugin_updater::Builder::new().build())
        .setup(move |app| {
            let app = app.handle().clone();
            let failure_app = app.clone();
            tauri::async_runtime::spawn(async move {
                let result = tokio::time::timeout(Duration::from_secs(180), async {
                    let state = service
                        .check(&app)
                        .await
                        .map_err(|error| error.to_string())?;
                    if state.status != UpdateStatus::Available {
                        return Err("No signed fixture update available".into());
                    }
                    report.record("real updater manifest check returned available")?;
                    service
                        .download(&app)
                        .await
                        .map_err(|error| error.to_string())?;
                    if service.get().status != UpdateStatus::Ready {
                        return Err("Download did not reach ready".into());
                    }
                    report.record("signed HTTPS installer streamed, authenticated, and staged")?;
                    // Successful Windows updater handoff exits this process.
                    service
                        .install(&app)
                        .await
                        .map_err(|error| error.to_string())?;
                    Err::<(), String>("Installer unexpectedly returned without exiting".into())
                })
                .await;
                let error = match result {
                    Ok(Err(error)) => error,
                    Err(_) => "Updater integration timed out".into(),
                    Ok(Ok(())) => "Unexpected updater result".into(),
                };
                report.fail(&error);
                failure_app.exit(1);
            });
            Ok(())
        })
        .build(context)?;
    application.run(|_, event| if matches!(event, RunEvent::Exit) {});
    Ok(())
}

struct Report(Mutex<Value>);
impl Report {
    fn record(&self, message: &str) -> Result<(), String> {
        let mut value = self.0.lock().unwrap();
        value["checks"].as_array_mut().unwrap().push(json!(message));
        std::fs::write(OUTPUT, serde_json::to_vec_pretty(&*value).unwrap())
            .map_err(|error| error.to_string())
    }
    fn fail(&self, error: &str) {
        let mut value = self.0.lock().unwrap();
        value["error"] = json!(error);
        let _ = std::fs::write(OUTPUT, serde_json::to_vec_pretty(&*value).unwrap());
    }
}
struct SmokeGate(Arc<Report>);
impl UpdateGate for SmokeGate {
    fn block_reason(&self, _: UpdateOperation) -> Option<String> {
        None
    }
    fn reserve_installation(&self) -> Result<Box<dyn Send>, String> {
        self.0
            .record("installation admission reserved after signature re-verification")?;
        Ok(Box::new(()))
    }
    fn before_exit(&self) {
        // This proves the real plugin reached its extraction/handoff hook, not
        // that installation succeeded. The guest wrapper verifies installed bytes.
        let _ = self
            .0
            .record("native updater reached installer handoff hook");
    }
}
