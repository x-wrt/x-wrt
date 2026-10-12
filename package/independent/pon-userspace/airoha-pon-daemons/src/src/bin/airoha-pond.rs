// SPDX-License-Identifier: GPL-2.0-only

use std::process::ExitCode;

fn main() -> ExitCode {
    airoha_pon_daemons::agent::run()
}
