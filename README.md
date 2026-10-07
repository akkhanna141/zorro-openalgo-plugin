# OpenAlgo broker plugin for Zorro

A broker plugin (C++, Win32 DLL) that connects [Zorro](https://zorro-project.com)
to [OpenAlgo](https://openalgo.in) — an open-source, self-hosted algo-trading
bridge that exposes a local REST API (`http://127.0.0.1:5000/api/v1`) to Indian
brokers such as Zerodha, Finvasia (Shoonya), Kotak Neo, Angel One, Flattrade,
Dhan, and others.

The plugin is **broker-agnostic**: the active broker is selected in the OpenAlgo
UI, not in the DLL. Any OpenAlgo user with a Zorro installation can use this
plugin by simply entering their OpenAlgo API key.

## Features

- Genuine C++ plugin with `extern "C"` exported API; builds as 32-bit and 64-bit
- Full Zorro broker API:
  - `BrokerOpen`, `BrokerLogin`, `BrokerTime`, `BrokerAccount`
  - `BrokerAsset` — live quotes; per-asset pip / lot / margin from a CSV asset list
  - `BrokerHistory2` — minute-bar history (falls back to cached local `.t6` data
    when the broker does not serve history, e.g. Kotak Neo)
  - `BrokerBuy2` / `BrokerSell2` / `BrokerTrade` — market and limit orders,
    partial closes, position reconciliation
  - `BrokerCommand`, including a local extension (`SET_OPTCONTRACT`, id 201) to
    pass option strike / expiry / call-or-put, since Indian option class symbols
    (e.g. `NIFTY22SEP2623250CE`) exceed Zorro's contract text slots
- Daily-tested against a live OpenAlgo installation with real orders, option
  trades, and position reconciliation. Recent hardening (v0.20–v0.24):
  - `BrokerHistory2` guards against dead zero-range candles (exchange closing
    prints / auction freezes) that corrupt ATR
  - Position-state persistence: the position map is saved per symbol
    (`Plugin\posstate_<keychk>_<sym>.csv`, keyed by a checksum of the API key —
    never the key itself) and restored + reconciled against the live broker
    positionbook at login, so a Zorro relogin can no longer orphan open
    positions
  - Entry-vs-close classification by order direction relative to net position
  - Exchange-side SL-M protective backstop orders (`SET_PROTSTOP`, broker
    command 2020): symbol-explicit publish format prevents crossed-symbol
    stops when Zorro batches `BrokerAsset` calls

> **Note:** `posstate_*.csv` files are generated at runtime and hold live
> trading state. They are excluded from version control and are not part of
> the source.

## Requirements

- [Zorro](https://zorro-project.com) (any recent version; headers come from the
  Zorro installation's `include` folder — `zorro.h`, `trading.h`, `functions.h`,
  `variables.h`)
- [OpenAlgo](https://docs.openalgo.in) installed and running locally
  (default REST endpoint: `http://127.0.0.1:5000/api/v1`)
- Visual Studio 2022 (or 2019) Build Tools with the C++ workload, for building

## Build

```
build_openalgo.bat
```

The script builds `bin\OpenAlgo.dll` (32-bit) and `bin64\OpenAlgo.dll` (64-bit)
from this folder. By default it expects this folder to sit inside a Zorro
installation; otherwise set `ZORRO_DIR` to the Zorro installation folder:

```
set ZORRO_DIR=C:\Path\To\Zorro
build_openalgo.bat
```

## Install

- `bin\OpenAlgo.dll`      → `Zorro\Plugin\`       (for Zorro.exe, 32-bit)
- `bin64\OpenAlgo.dll`    → `Zorro\Plugin64\`     (for Zorro64.exe, 64-bit)
- `Plugin\OpenAlgoAssets.csv` → `Zorro\Plugin\`

The plugin then appears as **OpenAlgo** in Zorro's `[Broker/Account]` scrollbox.

## Configuration

- **API key:** enter your OpenAlgo API key in Zorro's `[User]` field before
  logging in. The key is used at runtime only — it is never stored in this
  repository or in the DLL.
- **Endpoint:** the REST base URL is `http://127.0.0.1:5000/api/v1/` (the
  OpenAlgo default). Change `BASE_URL` in `Source\VC++\OpenAlgo.cpp` and rebuild
  if your OpenAlgo instance runs on another port.
- **Account type:** `Demo` maps to OpenAlgo's sandbox / analyze mode; `Real`
  places live orders.
- **Assets:** `Plugin\OpenAlgoAssets.csv` defines the asset list Zorro uses —
  symbol name, exchange, lot amount, pip and pip cost, margin, product code
  (e.g. `MIS`), and the full API symbol as the broker/master contract knows it.
  Edit it for your own instruments.

## Disclaimer

This software is provided as-is, without warranty of any kind. Trading
derivatives involves substantial risk of loss. Test thoroughly in OpenAlgo's
sandbox / analyze mode before placing live orders. The plugin is maintained by
the Zorro user community and is not subject to Zorro / oP group support.

## License

Released under the [MIT License](LICENSE) for the Zorro and OpenAlgo user
communities.
