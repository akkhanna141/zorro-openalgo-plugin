// OpenAlgo broker plugin for Zorro ////////////////////////////
// Bridges Zorro [Trade] to OpenAlgo's local REST API
// (http://127.0.0.1:5000/api/v1). All requests are POST JSON and carry
// the OpenAlgo apikey in the body. Broker-agnostic: the active broker
// (Zerodha/Kotak/...) is selected in the OpenAlgo UI, not in this DLL.
// Link with ZorroDLL.cpp.
//
// NOTE: variables.h (pulled in via zorro.h) defines lite-C macros that
// expand bare tokens like Margin, Amount, Account, Exchange, Fill,
// LotAmount, Asset, PIP into g->... expressions. We therefore avoid
// those exact tokens and use prefixed/suffixed names below.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <math.h>
#include <oleauto.h>
#include <zorro.h>

#pragma comment(lib, "oleaut32.lib")

#define PLUGIN_TYPE     2
#define PLUGIN_NAME     "OpenAlgo"
#define PLUGIN_VERSION  "0.30"

// plugin-local SET_OPTCONTRACT command id (brokerCommand): homegrown
// extension, not in Zorro's trading.h. 200+ stays clear of Zorro's own
// brokerCommand ids (trading.h tops out ~140). Scripts re-declare the value.
#define SET_OPTCONTRACT 201
#define DLLFUNC extern "C" __declspec(dllexport)
#define BASE_URL        "http://127.0.0.1:5000/api/v1/"

// v0.23 forward declarations (SL-M protective backstop helpers, defined
// after ltpOf; used earlier by BrokerSell2 / BrokerTrade / BrokerCommand)
int symPosRow(const char* Sym);
int symPosCreate(const char* Sym);
int orderIsComplete(const char* Oid);
int protCancel(int Row);
int protCancelId(const char* Oid, const char* Sym);
void protPlace(const char* Sym, double Level, int SignedQty);
void posStateSaveSym(const char* Sym);

// Global vars ///////////////////////////////////////////////////////////

int (__cdecl *BrokerMessage)(const char *Text) = NULL;
int (__cdecl *BrokerProgress)(intptr_t Progress) = NULL;

// Per-asset configuration loaded from Plugin\OpenAlgoAssets.csv
struct ASSETCFG {
    char Name[32];
    char Exch[16];
    double LotAmt;
    double PipVal;
    double PipCostVal;
    double MgnCost;
    char ProductCode[16];
    char ApiSym[32];    // symbol as OpenAlgo/master contract knows it
};                      // (empty -> uppercase Name)

#define MAX_ASSETS_CFG 64
#define MAX_SYMPOS 16

struct GLOBAL {
    int Diag;
    int HttpId;
    char Key[256];      // OpenAlgo apikey (Zorro [User] field)
    char Symbol[64];    // current asset (lowercase, as Zorro passes it)
    char Uuid[64];      // last placed order id
    double Unit;        // lot amount override (SET_AMOUNT)
    ASSETCFG AssetTbl[MAX_ASSETS_CFG];
    int NAssets;
    int IsDemo;         // 1 when OpenAlgo is in sandbox/analyze mode
    char TradeIds[10][64]; // slot -> Kotak order id (15+ digits overflow int)
    int IdxNext;        // round-robin cursor for TradeIds
    int LastSlot;       // slot of the last placed order (+1) = pseudo trade id
    DWORD LastOrderTick; // tick count of the last placed order (fill-lag gate)
    // Zorro option trades: the CONTRACT text slots are too small for the 19-char
    // Kotak class symbol (NIFTY22SEP2623250CE), so the script hands strike /
    // expiry / CE|PE over via brokerCommand(SET_OPTCONTRACT, "strike,exp,CE")
    // and BrokerBuy2 composes the full class symbol from it.
    char OptStrike[16]; // strike price as text, f.i. "23250"
    char OptExpiry[16]; // YYYYMMDD
    char OptType[4];    // "CE" or "PE"
    // Own-position map (plugin-side, per symbol): quantity the plugin holds,
    // sign-aware (+ long /- short), accumulated from placed orders (signed
    // Vol: entries add, closes subtract - exactly once each).
    // Used to clamp closes so Zorro can never sell more than it holds.
    struct {
        char Sym[32];
        int  Qty;       // signed: + long, - short
        int  Cnt;       // open trade count for the per-trade split (below)
        char ProtId[64];   // v0.23: exchange-side SL-M backstop order id
        double ProtLevel;  // published SL-M trigger price
        int    ProtQty;    // published signed qty (+SELL / -BUY)
    } SymPos[MAX_SYMPOS];
    // Per-quote sanity gate (v0.26): one slot per symbol. A price outside
    // the day's broker-truth band [low - 0.5%, high + 0.5%] is treated as a
    // feed artifact; the last known-good price is served instead, so
    // transient connectivity issues cannot distort indicators.
    // The gate self-heals: after 3 consecutive out-of-band polls it
    // re-anchors to accept genuine moves.
    struct {
        char Sym[32];
        double LastGood;    // last accepted price served to Zorro (0 = none)
        int    Rejects;     // consecutive out-of-band polls (self-heal)
    } QuoteGate[MAX_SYMPOS];
    // Per-trade quantity rule (v0.18): Zorro calls BrokerSell2/
    // BrokerTrade with its OWN ids (8-digit, observed 01-10), which this
    // plugin cannot map to its pseudo slots. But the split-exit design
    // always opens EQUAL halves, so per-trade qty = SymPos.Qty / SymPos.Cnt.
    // BrokerTrade reports that (kills `filled 20 of 10`), BrokerSell2 clamps
    // closes to it, and Cnt decrements per close. Equal halves make this
    // exact; the position map remains the hard safety net regardless.
    // Exchanges subscribed this session (v0.28): BrokerTime is global (no
    // asset argument), so the session's asset subscriptions decide which
    // market clock applies - an NSE-only strategy gets NSE hours, an MCX
    // strategy gets MCX hours. Wiped at login by memset(&G,0,...).
    char ExchSet[8][16];
    int  NExch;
} G;

// Utility functions /////////////////////////////////////////////////////////

// display message on the Zorro log
void showMsg(const char* Text, const char *Detail)
{
    static char Msg[4096];
    sprintf_s(Msg, "%s %s", Text, Detail);
    if (BrokerMessage) BrokerMessage(Msg);
}

// log a failed REST call with the raw response (truncated)
void failLog(const char* Text, const char* Response)
{
    static char Msg[512];
    if (Response && *Response)
        sprintf_s(Msg, "%.400s", Response);
    else
        sprintf_s(Msg, "(no response)");
    showMsg(Text, Msg);
}

// keep Zorro responsive
int sleep(int ms)
{
    Sleep(ms);
    return BrokerProgress ? BrokerProgress(0) : 1;
}

// 64 bit integer to text
char* i64toa(__int64 n)
{
    static char buffer[64];
    if (0 != _i64toa_s(n, buffer, 64, 10)) *buffer = 0;
    return buffer;
}

// logged in?
inline BOOL isConnected()
{
    return *G.Key != 0;
}

// uppercase a symbol in place into a static buffer
char* upperSymbol(const char* Sym)
{
    static char Buf[64];
    int i = 0;
    for (; Sym && Sym[i] && i < 63; i++)
        Buf[i] = (char)toupper((unsigned char)Sym[i]);
    Buf[i] = 0;
    return Buf;
}

// find asset config by (lowercase) name; NULL if not found
ASSETCFG* findAsset(const char* Name)
{
    for (int i = 0; i < G.NAssets; i++)
        if (0 == strcmpi(G.AssetTbl[i].Name, Name))
            return &G.AssetTbl[i];
    return NULL;
}

// ------- position-state persistence (v0.22) ////////////////////////////
// The plugin state (position map) lives in RAM, and Zorro relogins occur
// on every session start, reconnect, and SET_RESTART. State is therefore
// persisted after every order and restored + reconciled against the live
// broker positionbook at login, so open positions stay managed across
// relogins.
// v0.23: ONE FILE PER SYMBOL (Plugin\posstate_<keychk>_<sym>.csv), suffixed
// with a checksum of the apikey - never the key itself. Two Zorro
// instances on different accounts (or trading disjoint symbol sets on the
// same account) never overwrite each other's state. Each file carries the
// position map row AND the SL-M backstop order id.

// per-symbol posstate path: <exedir>\Plugin\posstate_<keychk>_<sym>.csv
void posStatePath(char* Path, int Max, const char* Sym)
{
    GetModuleFileNameA(NULL, Path, Max);
    char* Slash = strrchr(Path, '\\');
    if (Slash) *(Slash + 1) = 0;
    char Base[MAX_PATH];
    strcpy_s(Base, Path);
    // key checksum (never the key itself)
    unsigned Chk = 5381;
    for (const char* p = G.Key; *p; p++)
        Chk = ((Chk << 5) + Chk) ^ (unsigned char)*p;
    char Low[32];
    int i = 0;
    for (; Sym[i] && i < 31; i++)
        Low[i] = (char)tolower((unsigned char)Sym[i]);
    Low[i] = 0;
    sprintf_s(Path, Max, "%sPlugin\\posstate_%05u_%s.csv", Base, Chk % 100000, Low);
}

// persist one symbol's row; an empty row deletes the file
void posStateSaveSym(const char* Sym)
{
    char Path[MAX_PATH];
    posStatePath(Path, MAX_PATH, Sym);
    int Row = symPosRow(Sym);
    int Empty = (Row < 0) || (G.SymPos[Row].Qty == 0 && G.SymPos[Row].Cnt == 0
                              && !G.SymPos[Row].ProtId[0]);
    if (Empty) {
        DeleteFileA(Path);
        return;
    }
    char Tmp[MAX_PATH];
    strcpy_s(Tmp, Path);
    strcat_s(Tmp, ".tmp");
    FILE* f = NULL;
    if (0 != fopen_s(&f, Tmp, "w") || !f) {
        if (G.Diag >= 1) showMsg("posstate save failed:", Tmp);
        return;
    }
    fprintf(f, "%s,%d,%d,%s,%.2f,%d\n", G.SymPos[Row].Sym, G.SymPos[Row].Qty,
        G.SymPos[Row].Cnt, G.SymPos[Row].ProtId, G.SymPos[Row].ProtLevel,
        G.SymPos[Row].ProtQty);
    fclose(f);
    MoveFileExA(Tmp, Path, MOVEFILE_REPLACE_EXISTING);
    if (G.Diag >= 1) showMsg("posstate saved:", Path);
}

// persist all rows (write changed files, delete files of emptied rows)
void posStateSave()
{
    for (int i = 0; i < MAX_SYMPOS; i++)
        if (G.SymPos[i].Sym[0]) posStateSaveSym(G.SymPos[i].Sym);
}

// named struct for the persisted position row (file + reconcile buffer)
struct POSROW {
    char Sym[32];
    int  Qty;
    int  Cnt;
    char ProtId[64];   // v0.23: SL-M backstop order id
    double ProtLevel;
    int    ProtQty;
};

// load the position map from all per-symbol posstate files.
// Returns the number of rows loaded (0 = no files / empty).
int posStateLoadAll(POSROW* Out, int MaxRows)
{
    char Path[MAX_PATH];
    GetModuleFileNameA(NULL, Path, MAX_PATH);
    char* Slash = strrchr(Path, '\\');
    if (Slash) *(Slash + 1) = 0;
    unsigned Chk = 5381;
    for (const char* p = G.Key; *p; p++)
        Chk = ((Chk << 5) + Chk) ^ (unsigned char)*p;
    char Mask[MAX_PATH];
    sprintf_s(Mask, sizeof(Mask), "%sPlugin\\posstate_%05u_*.csv", Path, Chk % 100000);
    WIN32_FIND_DATAA FD;
    HANDLE H = FindFirstFileA(Mask, &FD);
    if (H == INVALID_HANDLE_VALUE) return 0;
    int N = 0;
    do {
        if (N >= MaxRows) break;
        char Full[MAX_PATH];
        sprintf_s(Full, sizeof(Full), "%sPlugin\\%s", Path, FD.cFileName);
        FILE* f = NULL;
        if (0 != fopen_s(&f, Full, "r") || !f) continue;
        char Line[192];
        while (fgets(Line, sizeof(Line), f)) {
            char Name[32]; int Qty = 0, Cnt = 0;
            char Prot[64] = ""; double Lvl = 0.; int PQ = 0;
            int Fields = sscanf_s(Line, " %31[^,],%d,%d,%63[^,],%lf,%d",
                Name, (unsigned)sizeof(Name), &Qty, &Cnt,
                Prot, (unsigned)sizeof(Prot), &Lvl, &PQ);
            if (Fields < 3) continue;
            strcpy_s(Out[N].Sym, Name);
            Out[N].Qty = Qty;
            Out[N].Cnt = Cnt;
            strcpy_s(Out[N].ProtId, Fields >= 4 ? Prot : "");
            Out[N].ProtLevel = Fields >= 5 ? Lvl : 0.;
            Out[N].ProtQty = Fields >= 6 ? PQ : 0;
            N++;
            break;
        }
        fclose(f);
    } while (FindNextFileA(H, &FD));
    FindClose(H);
    return N;
}

// ------- own-position map & per-trade registry //////////////////////////
// The plugin tracks the quantity IT placed per symbol and the remaining
// quantity of each open trade slot, so quantities reported to Zorro and
// quantities submitted to the broker always match what Zorro actually owns.

// ------- own-position map (v0.18) ///////////////////////////////////////
// The plugin tracks what IT placed per symbol: signed quantity (+ long /
// - short) and the count of open trades. Entries add qty and count; closes
// subtract BOTH (exactly once). Attempts to
// close more than held are clamped/suppressed at submit time.

// add an order: classify ENTRY vs CLOSE by the order direction RELATIVE to
// the current net position, not by raw signed qty (v0.21), so short
// entries are tracked correctly. Rules (symmetric for long and short):
//   SELL when Owned <= 0  -> short ENTRY  (net more negative, Cnt++)
//   SELL when Owned > 0   -> long CLOSE   (Cnt--)
//   BUY  when Owned >= 0  -> long ENTRY   (net more positive, Cnt++)
//   BUY  when Owned < 0   -> short CLOSE  (Cnt--)
void symPosAdd(const char* Sym, int SignedQty)
{
    if (!Sym || !*Sym || !SignedQty) return;
    for (int i = 0; i < MAX_SYMPOS; i++) {
        if (!G.SymPos[i].Sym[0] || 0 == strcmpi(G.SymPos[i].Sym, Sym)) {
            if (!G.SymPos[i].Sym[0]) {
                strcpy_s(G.SymPos[i].Sym, upperSymbol(Sym));
                G.SymPos[i].Qty = 0;
                G.SymPos[i].Cnt = 0;
            }
            int Owned = G.SymPos[i].Qty;
            // v0.21: direction-relative entry/close classification
            int IsEntry;
            if (SignedQty < 0) IsEntry = (Owned <= 0); // SELL: entry vs close
            else               IsEntry = (Owned >= 0); // BUY: entry vs close
            G.SymPos[i].Qty += SignedQty;
            if (IsEntry) G.SymPos[i].Cnt++;
            else if (G.SymPos[i].Cnt > 0) G.SymPos[i].Cnt--;
            if (G.Diag >= 1) {
                char Db[96];
                sprintf_s(Db, "%s %+d -> net %d, %d open (%s)",
                    G.SymPos[i].Sym, SignedQty, G.SymPos[i].Qty, G.SymPos[i].Cnt,
                    IsEntry ? "ENTRY" : "CLOSE");
                showMsg("own-position map:", Db);
                }
            return;
        }
    }
    if (G.Diag >= 1) showMsg("own-position map FULL, not tracked:", Sym);
}

// current mapped position (signed) for a symbol
int symPosGet(const char* Sym)
{
    for (int i = 0; i < MAX_SYMPOS; i++)
        if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, Sym))
            return G.SymPos[i].Qty;
    return 0;
}

// apply a close: subtract signed close qty and one open trade, exactly once
void symPosApplyClose(const char* Sym, int SignedClose)
{
    for (int i = 0; i < MAX_SYMPOS; i++)
        if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, Sym)) {
            G.SymPos[i].Qty += SignedClose;
            if (G.SymPos[i].Cnt > 0) G.SymPos[i].Cnt--;
            if (G.Diag >= 1) {
                char Db[96];
                sprintf_s(Db, "%s close %+d -> net %d, %d open",
                    G.SymPos[i].Sym, SignedClose, G.SymPos[i].Qty, G.SymPos[i].Cnt);
                showMsg("own-position map:", Db);
            }
            return;
        }
}

// largest safe close for a symbol, signed. SignedClose > 0 = BUY (closing
// a short), < 0 = SELL (closing a long). Returns 0 = suppress the close.
// v0.21: sign-aware per-trade share. v0.18 computed PerTrade = Owned/Cnt
// and rejected a SELL close whenever PerTrade <= 0 - which suppressed
// BUY-backs of shorts whenever the map was wrong, and would have allowed
// closing a long with a BUY (reverse direction) had Cnt/Qty disagreed.
// Now the close direction must OPPOSITE the net position sign:
//   SELL close valid only when Owned > 0 (long)
//   BUY close valid only when Owned < 0 (short)
// and the per-trade share is |Owned| / Cnt in the close direction.
int clampClose(const char* Sym, int SignedClose)
{
    int Owned = 0, Cnt = 0;
    for (int i = 0; i < MAX_SYMPOS; i++)
        if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, Sym)) {
            Owned = G.SymPos[i].Qty; Cnt = G.SymPos[i].Cnt; break;
        }
    if (SignedClose < 0) {           // SELL close: valid for a LONG position
        if (Owned <= 0) return 0;    // nothing long to close (v0.21: was PerTrade<=0)
        int PerTrade = (Cnt > 1) ? (Owned / Cnt) : Owned;
        return -min(-SignedClose, PerTrade);
    }
    if (SignedClose > 0) {           // BUY close: valid for a SHORT position
        if (Owned >= 0) return 0;    // nothing short to close (v0.21: was PerTrade>=0)
        int PerTrade = (Cnt > 1) ? (Owned / Cnt) : Owned;
        return min(SignedClose, -PerTrade);
    }
    return 0;
}

// resolve exchange for a symbol: CSV map, else default "NSE".
// NFO option symbols (strike + CE/PE suffix, e.g. NIFTY22SEP2623250CE) are
// traded on NFO, not NSE - Zorro contract trades carry the raw broker class
// string, which the CSV map has no row for. Option class symbols always end
// with the CE (call) or PE (put) suffix.
const char* exchangeOf(const char* Sym)
{
    ASSETCFG* A = findAsset(Sym);
    if (A && *A->Exch) return A->Exch;
    if (Sym && *Sym) {
        size_t Len = strlen(Sym);
        if (Len > 2 && (0 == strcmp(Sym + Len - 2, "CE") || 0 == strcmp(Sym + Len - 2, "PE")))
            return "NFO";
    }
    return "NSE";
}

// resolve product for a symbol: CSV map, else default "MIS"
const char* productOf(const char* Sym)
{
    ASSETCFG* A = findAsset(Sym);
    if (A && *A->ProductCode) return A->ProductCode;
    return "MIS";
}

// compose the full Kotak option class symbol from the SET_OPTCONTRACT store:
// NIFTY<DDMMMYY><strike><CE|PE>, f.i. NIFTY22SEP2623250CE (verified format of
// the live /optionchain symbols; matching symbols resolve on /placeorder).
char* optClassSymbol()
{
    static char Buf[64];
    Buf[0] = 0;
    if (!G.OptStrike[0] || !G.OptType[0]) return Buf;
    int Exp = atoi(G.OptExpiry);            // YYYYMMDD
    if (Exp < 20200101) return Buf;
    static const char* MON[] = { "JAN","FEB","MAR","APR","MAY","JUN",
                                 "JUL","AUG","SEP","OCT","NOV","DEC" };
    int Y = Exp / 10000, M = (Exp / 100) % 100, D = Exp % 100;
    if (M < 1 || M > 12) return Buf;
    sprintf_s(Buf, "NIFTY%02d%s%02d%s%s", D, MON[M - 1], Y % 100, G.OptStrike, G.OptType);
    return Buf;
}

// resolve the symbol OpenAlgo/the master contract expects: CSV "Symbol"
// column override (e.g. nifty50 -> NIFTY, crudeoilm26julfut -> CRUDEOIL...),
// else the uppercased Zorro asset name
const char* apiSymbolOf(const char* Sym)
{
    ASSETCFG* A = findAsset(Sym);
    if (A && *A->ApiSym) return A->ApiSym;
    return upperSymbol(Sym);
}

// load Plugin\OpenAlgoAssets.csv into G.AssetTbl
// Columns: Name,Exchange,LotAmount,Pip,PipCost,Margin,Product[,Symbol]
void loadAssetCSV()
{
    G.NAssets = 0;
    char Path[MAX_PATH];
    GetModuleFileNameA(NULL, Path, MAX_PATH); // Zorro.exe dir
    char* Slash = strrchr(Path, '\\');
    if (Slash) *(Slash + 1) = 0;
    strcat_s(Path, "Plugin\\OpenAlgoAssets.csv");

    FILE* f = NULL;
    if (0 != fopen_s(&f, Path, "r") || !f) return; // optional file
    char Line[512];
    int First = 1;
    while (fgets(Line, sizeof(Line), f)) {
        if (First) { First = 0; continue; } // skip header
        if (G.NAssets >= MAX_ASSETS_CFG) break;
        ASSETCFG A;
        memset(&A, 0, sizeof(A));
        A.LotAmt = 1.; A.PipVal = 0.05; A.PipCostVal = 0.05; A.MgnCost = -5;
        strcpy_s(A.Exch, "NSE"); strcpy_s(A.ProductCode, "MIS");
        char Exch[16], Prod[16], Name[32], ApiSym[32];
        double Lot, Pip, PipCost, Mgn;
        int n = sscanf_s(Line, " %31[^,],%15[^,],%lf,%lf,%lf,%lf,%15[^,],%31[^\n\r]",
            Name, (unsigned)sizeof(Name), Exch, (unsigned)sizeof(Exch),
            &Lot, &Pip, &PipCost, &Mgn, Prod, (unsigned)sizeof(Prod),
            ApiSym, (unsigned)sizeof(ApiSym));
        if (n < 1 || !*Name) continue;
        strcpy_s(A.Name, Name);
        if (n >= 2 && *Exch) strcpy_s(A.Exch, Exch);
        if (n >= 3 && Lot > 0) A.LotAmt = Lot;
        if (n >= 4 && Pip > 0) A.PipVal = Pip;
        if (n >= 5 && PipCost >= 0) A.PipCostVal = PipCost;
        if (n >= 6) A.MgnCost = Mgn;
        if (n >= 7 && *Prod) strcpy_s(A.ProductCode, Prod);
        if (n >= 8 && *ApiSym) strcpy_s(A.ApiSym, ApiSym);
        G.AssetTbl[G.NAssets++] = A;
    }
    fclose(f);
}

// Build the full OpenAlgo request body, injecting the apikey.
// `Inner` is the JSON key-value content without braces, e.g.:
//   "symbol":"RELIANCE","exchange":"NSE"
// Pass NULL/"" for a body that only contains the apikey.
char* buildBody(const char* Inner)
{
    static char Body[4096];
    if (Inner && *Inner)
        sprintf_s(Body, "{\"apikey\":\"%s\",%s}", G.Key, Inner);
    else
        sprintf_s(Body, "{\"apikey\":\"%s\"}", G.Key);
    return Body;
}

// send a POST JSON request to the OpenAlgo API.
// `Inner` is inner JSON body content (apikey injected automatically).
// Returns the response buffer, or NULL on transfer failure.
// Mode & 2: use the small secondary buffer.
char* send(const char* Path, const char* Inner, int Mode = 0)
{
    static char URL[1024], Header[256],
        Buffer1[1024 * 1024], Buffer2[2048];
    *Header = 0;
    sprintf_s(URL, "%s%s", BASE_URL, Path);
    char* Response = (Mode & 2) ? Buffer2 : Buffer1;
    int MaxSize = (Mode & 2) ? sizeof(Buffer2) : sizeof(Buffer1);

    strcpy_s(Header, "Content-Type:application/json");
    char* Body = buildBody(Inner);

    if (G.Diag >= 2)
        showMsg("Send:", Path);

    int Id = http_request(URL, Body, Header, "POST");
    if (!Id) goto send_error;

    // wait up to ~30s for the server to reply
    int Size = 0, Wait = 3000;
    while (!(Size = http_status(Id)) && --Wait > 0) {
        if (!sleep(10)) goto send_error;
    }
    if (!Size) goto send_error;
    if (!http_result(Id, Response, MaxSize))
        goto send_error;
    Response[MaxSize - 1] = 0; // guard against overrun
    if (G.Diag >= 2)
        showMsg("Resp:", Response);
    http_free(Id); // OpenAlgo is request/response; close each call
    return Response;

send_error:
    if (Id) http_free(Id);
    G.HttpId = 0;
    if (G.Diag >= 1)
        showMsg("Failed:", Path);
    return NULL;
}

// Parse an OpenAlgo candle timestamp into a DATE.
// Handles ISO "YYYY-MM-DD HH:MM:SS[+05:30]" first, then numeric Unix.
int parseTimestamp(const char* Str, DATE* Out)
{
    if (!Str || !*Str) return 0;
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    int n = sscanf_s(Str, "%4hd-%2hd-%2hd %2hd:%2hd:%2hd",
        &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond);
    if (n < 5) // also handle "YYYY-MM-DDTHH:MM:SS"
        n = sscanf_s(Str, "%4hd-%2hd-%2hdT%2hd:%2hd:%2hd",
            &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond);
    if (n >= 5) {
        double v = 0;
        if (SystemTimeToVariantTime(&st, &v)) { *Out = v; return 1; }
    }
    // fall back: numeric Unix timestamp
    double unix = atof(Str);
    if (unix > 0) {
        *Out = unix / 86400. + 25569.;
        return 1;
    }
    return 0;
}

// ISO date YYYY-MM-DD from a DATE
void dateToISO(DATE D, char* Out, int OutLen)
{
    SYSTEMTIME st;
    VariantTimeToSystemTime(D, &st);
    sprintf_s(Out, OutLen, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

// returns 1 if the OpenAlgo response status == "success"
int statusOK(const char* Response)
{
    if (!Response) return 0;
    char* Status = strtext(Response, "status", "");
    return (0 == strcmpi(Status, "success"));
}

// Fall back to the local frozen .t6 file when OpenAlgo /history is unavailable.
// This keeps the COEXIST principle: the same .t6 history (refreshed by
// zorro_refresh.py) powers [Test]/[Train] AND the [Trade] lookback. Live
// current bars continue to build from BrokerAsset quotes.
// .t6 record = DATE time + 6 floats (fHigh,fLow,fOpen,fClose,fVal,fVol) = 32B.
// time is the bar CLOSE timestamp (Zorro/GMT convention) — no span adjustment.
// Returns bars in REVERSE order (most-recent first), capped at NTicks, within
// [Start,End].
// File suffix: try the requested interval first, then fall back to "5m"
// (the standard file produced by the ConvertIndia5min pipeline). Some brokers
// (e.g. Kotak Neo) serve no history at all, so the local .t6 is the only
// lookback source; Zorro may request 1m (set(TICKS)) but only 5m files exist.
int readT6History(const char* Symbol, const char* IntervalSuffix,
    DATE Start, DATE End, int NTicks, T6* Ticks)
{
    if (!Symbol || !Ticks || !NTicks) return 0;
    // candidate suffixes: requested interval, then the standard "5m"
    const char* Suffixes[2];
    Suffixes[0] = IntervalSuffix && *IntervalSuffix ? IntervalSuffix : "5m";
    Suffixes[1] = "5m";

    for (int s = 0; s < 2; s++) {
        // resolve History\<symbol>_<suffix>.t6 relative to Zorro.exe dir
        char Path[MAX_PATH];
        GetModuleFileNameA(NULL, Path, MAX_PATH);
        char* Slash = strrchr(Path, '\\');
        if (Slash) *(Slash + 1) = 0;
        strcat_s(Path, "History\\");
        // lowercase asset name for the filename (AssetsIndia.csv is lowercase)
        char Low[32]; int i = 0;
        for (; Symbol[i] && i < 31; i++) Low[i] = (char)tolower((unsigned char)Symbol[i]);
        Low[i] = 0;
        strcat_s(Path, Low);
        strcat_s(Path, "_");
        strcat_s(Path, Suffixes[s]);
        strcat_s(Path, ".t6");

        FILE* f = NULL;
        if (0 != fopen_s(&f, Path, "rb") || !f) continue; // try next suffix

        // read all records into a growable buffer
        fseek(f, 0, SEEK_END);
        long Bytes = ftell(f);
        fseek(f, 0, SEEK_SET);
        int Total = Bytes / (int)sizeof(T6);
        if (Total <= 0) { fclose(f); continue; }
        T6* Buf = (T6*)malloc(Bytes);
        if (!Buf) { fclose(f); return 0; }
        size_t Rd = fread(Buf, sizeof(T6), Total, f);
        fclose(f);
        int N = (int)Rd;
        // The frozen .t6 files (from ConvertIndia5min) are stored NEWEST-FIRST,
        // i.e. record 0 is the most recent bar. Walk forward in file order so
        // Ticks[0] = newest (Zorro wants most-recent-first). Cap with End (return
        // the most recent bars not exceeding End). We do not hard-break on Start,
        // because off-hours the requested window straddles a non-trading gap;
        // returning the most recent N bars is the standard broker behavior.
        //
        // v0.25: the End request from Zorro can be stale (Zorro re-stamps
        // lookback bars by count from 'now'), so it is used only as a
        // secondary filter, never as a hard truncation. The .t6 file,
        // refreshed daily, is the freshest source of truth: serve the
        // NEWEST NTicks bars from the file head. Bars newer than BOTH End
        // and the file's newest bar would only exist if the request were
        // from the future - never skip file-head bars for being "newer
        // than End". Start is likewise not hard-enforced (straddles gaps).
        //
        // TIME ZONE: .t6 files produced by ConvertIndia5min (v2026-09-09)
        // store ZORRO TIME (UTC): the conversion script subtracts IST 5h30m
        // from the broker's IST candles. Older IST-stamped files are NOT
        // shifted again here (that would double-shift the lookback); if a
        // stale pre-09-09 .t6 is ever read, refresh it through
        // openalgo_refresh.py + ConvertIndia5min instead.
        const double IST_OFFSET = 0.0;
        int Received = 0;
        // v0.25: newest-first file walk - take bars from the file HEAD
        // (newest) until NTicks is filled. End is NOT enforced: a stale
        // request End must not truncate the freshest data. (The REST branch
        // above keeps its End clamp - broker-served candles are already
        // windowed by the request's own start/end dates.)
        for (int k = 0; k < N && Received < NTicks; k++) {
            DATE t = Buf[k].time - IST_OFFSET;  // IST -> UTC
            // v0.20: skip dead bars (O==H==L==C, zero range) - same guard
            // as the REST branch. Zero-range bars (exchange closing prints /
            // auction freezes) carry no tradable information and distort
            // range-based indicators such as ATR.
            if (Buf[k].fOpen == Buf[k].fHigh && Buf[k].fHigh == Buf[k].fLow
                && Buf[k].fLow == Buf[k].fClose)
                continue;
            Ticks[Received].time   = t;
            Ticks[Received].fHigh  = Buf[k].fHigh;
            Ticks[Received].fLow   = Buf[k].fLow;
            Ticks[Received].fOpen  = Buf[k].fOpen;
            Ticks[Received].fClose = Buf[k].fClose;
            Ticks[Received].fVal   = Buf[k].fVal;
            Ticks[Received].fVol   = Buf[k].fVol;
            Received++;
        }
        // v0.19 FIX: Zorro's live-history loader expects Ticks in CHRONOLOGICAL
        // order (oldest first, newest adjacent to 'now') and re-stamps the
        // lookback by count. We filled newest-first (the .t6 FILE convention -
        // a different code path), so Zorro's lookback bars were paired with the
        // file's OLDEST records. Reverse in place: Ticks[0] = oldest,
        // Ticks[Received-1] = newest.
        for (int a = 0, b = Received - 1; a < b; a++, b--) {
            T6 Tmp = Ticks[a]; Ticks[a] = Ticks[b]; Ticks[b] = Tmp;
        }
        free(Buf);
        if (Received > 0) {
            if (G.Diag >= 1) {
                SYSTEMTIME sA, sB;
                VariantTimeToSystemTime(Ticks[0].time, &sA);
                VariantTimeToSystemTime(Ticks[Received-1].time, &sB);
                char Diag[192];
                sprintf_s(Diag, "History from local .t6: %s  %d bars  %04d-%02d-%02d %02d:%02d .. %04d-%02d-%02d %02d:%02d (chronological)",
                    Path, Received, sA.wYear, sA.wMonth, sA.wDay, sA.wHour, sA.wMinute,
                    sB.wYear, sB.wMonth, sB.wDay, sB.wHour, sB.wMinute);
                showMsg(Diag, "");
            }
            return Received;
        }
    }
    return 0;
}

//////////////////////////////////////////////////////////////

// ------- session clock (v0.28) ///////////////////////////////////////////
// BrokerTime is global (no asset argument), so the session's subscribed
// exchanges (recorded by BrokerAsset) decide which market clock applies.
// per-exchange trading windows (seconds of UTC day; IST = UTC+5:30):
//   NSE/BSE cash+derivates: 09:15:15-15:14:45 IST (v0.27 auction-trimmed
//   window: pre-open + closing auctions excluded - see BrokerTime note)
//   MCX:                    09:00:15-23:29:45 IST (non-agri session,
//   edge-trimmed like NSE: 09:00:00 opening snap and 23:30 close skipped)
static const int NSE_OPEN_S  = 3 * 3600 + 45 * 60 + 15;  // 03:45:15 UTC
static const int NSE_CLOSE_S = 9 * 3600 + 44 * 60 + 45;  // 09:44:45 UTC
static const int MCX_OPEN_S  = 3 * 3600 + 30 * 60 + 15;  // 03:30:15 UTC
static const int MCX_CLOSE_S = 17 * 3600 + 59 * 60 + 45; // 17:59:45 UTC

// record an exchange as subscribed this session (BrokerAsset calls this)
static void sessionExchAdd(const char* Exch)
{
    if (!Exch || !*Exch) return;
    // NSE/BSE/NSE_INDEX/BSE_INDEX/NFO/CDS all follow the NSE clock
    if (0 == strcmp(Exch, "MCX")) {
        for (int i = 0; i < G.NExch; i++)
            if (0 == strcmp(G.ExchSet[i], "MCX")) return;
        if (G.NExch < 8) strcpy_s(G.ExchSet[G.NExch++], "MCX");
        return;
    }
    for (int i = 0; i < G.NExch; i++)
        if (0 == strcmp(G.ExchSet[i], "NSE")) return;
    if (G.NExch < 8) strcpy_s(G.ExchSet[G.NExch++], "NSE");
}

// 1 when any subscribed exchange follows the MCX clock
static int sessionUsesMcx()
{
    for (int i = 0; i < G.NExch; i++)
        if (0 == strcmp(G.ExchSet[i], "MCX")) return 1;
    return 0;
}

// v0.29: market state of the subscribed exchanges' session clock.
// 1 = at least one subscribed exchange is inside its trading window
// (weekday + window open..close), 0 = all closed. Shared by BrokerTime and
// the BrokerAsset session gate (single source of truth for both edges).
static int sessionOpenNow()
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    int utcSec = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
    int OpenS, CloseS;
    if (sessionUsesMcx()) { OpenS = MCX_OPEN_S; CloseS = MCX_CLOSE_S; }
    else                  { OpenS = NSE_OPEN_S; CloseS = NSE_CLOSE_S; }
    return (st.wDayOfWeek >= 1 && st.wDayOfWeek <= 5
            && utcSec >= OpenS && utcSec < CloseS);
}

// most recent session close instant - superseded in v0.29 by the
// quote-path gate in BrokerAsset, which is the effective mechanism
// (see the BrokerTime and BrokerAsset notes). Function removed.

DLLFUNC int BrokerOpen(char* Name, FARPROC fpMessage, FARPROC fpProgress)
{
    strcpy_s(Name, 32, PLUGIN_NAME);
    (FARPROC&)BrokerMessage = fpMessage;
    (FARPROC&)BrokerProgress = fpProgress;
    return PLUGIN_TYPE;
}

DLLFUNC int BrokerTime(DATE* pTimeUTC)
{
    // v0.29 session clock (exchange-aware, per-exchange windows in
    // sessionExchAdd above):
    //   NSE/BSE: 09:15:15-15:14:45 IST = 03:45:15-09:44:45 UTC
    //   MCX:     09:00:15-23:29:45 IST = 03:30:15-17:59:45 UTC
    // (IST = UTC+5:30). The regulator's pre-open and closing-auction
    // windows sit at the NSE session edges; bars built from auction
    // prints misrepresent the traded range. The square-off runs at
    // 15:05 IST, so bars after 15:15 carry no trading value.
    //
    // Design contract: this function keeps serving real advancing UTC
    // time and reports open/closed state per the documented Zorro
    // contract (2 = open, 1 = closed-but-connected; never 0 = re-login
    // loop). Bar formation outside sessions is prevented by the
    // quote-path gate in BrokerAsset (v0.29), which is the effective
    // lever: Zorro completes a bar only when a price is served.
    SYSTEMTIME stUTC;
    GetSystemTime(&stUTC);                 // PC clock interpreted as UTC
    int MarketOpen = sessionOpenNow();     // single source of truth (v0.29)

    if (pTimeUTC) {
        // always serve the real advancing UTC time (per the documented
        // contract; see the BrokerTime note above)
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        SYSTEMTIME stNow;
        FileTimeToSystemTime(&ft, &stNow);
        SystemTimeToVariantTime(&stNow, pTimeUTC);
    }
    return MarketOpen ? 2 : 1;
}

DLLFUNC int BrokerLogin(char* User, char* Pwd, char* Type, char* Accounts)
{
    if (User) { // login
        memset(&G, 0, sizeof(G));
        strcpy_s(G.Key, User);
        loadAssetCSV();
        // Log the account type Zorro is using (Real vs Demo) for debugging
        // the free-version Error 074 (balance limit exceeded, real accounts only).
        {
            char TBuf[32];
            sprintf_s(TBuf, "Type=[%s]", Type ? Type : "(null)");
            showMsg("BrokerLogin", TBuf);
        }
        if (!*User) {
            showMsg("OpenAlgo plugin", PLUGIN_VERSION);
            return 1; // prices-only / no apikey
        }
        // validate apikey via /funds
        char* Response = send("funds", "");
        if (!Response) {
            showMsg("OpenAlgo not running on", BASE_URL);
            return 0;
        }
        if (!statusOK(Response)) {
            char* Msg = strtext(Response, "message", "invalid apikey");
            showMsg("Login failed:", Msg);
            return 0;
        }
        // Detect OpenAlgo sandbox/analyze mode from the optional "mode" field.
        // A live broker connection (e.g. Kotak) has NO "mode" field and reports a
        // real balance; the sandbox reports mode:"analyze" with ~Rs.1 Cr capital.
        // The leading '!' on the LIVE line makes Zorro pop an alert box so you can
        // never mistake a live session for sandbox when real money is at stake.
        char* Mode = strtext(Response, "mode", "");
        if (Mode && *Mode && (0 == strcmpi(Mode, "analyze") || 0 == strcmpi(Mode, "sandbox")))
            G.IsDemo = 1;
        double Bal = strvar(Response, "availablecash", 0.);
        {
            char BB[96];
            if (G.IsDemo)
                sprintf_s(BB, "OpenAlgo SANDBOX  Balance %.2f  (paper, no broker orders)", Bal);
            else
                sprintf_s(BB, "!OpenAlgo LIVE  Balance %.2f  orders route to REAL broker", Bal);
            showMsg(BB, "");
        }
        showMsg("OpenAlgo plugin", PLUGIN_VERSION);
        // v0.22: restore the position map saved by the previous session and
        // reconcile it against the LIVE broker positionbook. This closes the
        // reconnect hole: after any relogin (session start, wifi/power outage
        // recovery, SET_RESTART), the map was empty and BrokerTrade reported
        // every real open position as "closed" - unmanaged exposure, no stop.
        {
            POSROW Rows[MAX_SYMPOS];
            int NRows = posStateLoadAll(Rows, MAX_SYMPOS);
            // live broker net positions, for reconcile
            int BQty[MAX_SYMPOS]; char BSym[MAX_SYMPOS][32]; int NB = 0;
            char* Resp = send("positionbook", "", 2);
            if (Resp && statusOK(Resp)) {
                char* Scan = Resp;
                while (Scan && NB < MAX_SYMPOS) {
                    char* Row = strchr(Scan, '{');
                    if (!Row) break;
                    Scan = strchr(Row + 1, '{');
                    char* Sym = strtext(Row, "symbol", "");
                    double Qty = strvar(Row, "quantity", 0.);
                    if (*Sym && (int)Qty != 0) {
                        strcpy_s(BSym[NB], Sym);
                        BQty[NB] = (int)Qty;
                        NB++;
                    }
                }
            }
            int Restored = 0;
            for (int r = 0; r < NRows; r++) {
                // find the broker row for this symbol
                int B = -1;
                for (int b = 0; b < NB; b++)
                    if (0 == strcmpi(BSym[b], Rows[r].Sym)) { B = b; break; }
                if (B >= 0 && BQty[B] != 0) {
                    // broker holds a position: adopt the broker net as truth,
                    // keep the saved open-trade count for the per-trade split
                    strcpy_s(G.SymPos[Restored].Sym, Rows[r].Sym);
                    G.SymPos[Restored].Qty = BQty[B];
                    G.SymPos[Restored].Cnt = Rows[r].Cnt > 0 ? Rows[r].Cnt : 1;
                    G.SymPos[Restored].ProtId[0] = 0;
                    G.SymPos[Restored].ProtLevel = 0.;
                    G.SymPos[Restored].ProtQty = 0;
                    // v0.23: re-adopt a saved SL-M backstop. If it already
                    // FIRED while Zorro was down, the broker net above will
                    // reflect it on the next poll; if the order still stands,
                    // keep its id so it can be replaced/cancelled.
                    if (Rows[r].ProtId[0]) {
                        if (orderIsComplete(Rows[r].ProtId)) {
                            showMsg("posstate: SL-M FIRED while offline:",
                                Rows[r].Sym);
                        } else {
                            strcpy_s(G.SymPos[Restored].ProtId, Rows[r].ProtId);
                            G.SymPos[Restored].ProtLevel = Rows[r].ProtLevel;
                            G.SymPos[Restored].ProtQty = Rows[r].ProtQty;
                            if (G.Diag >= 1)
                                showMsg("posstate: SL-M backstop adopted:",
                                    Rows[r].ProtId);
                        }
                    }
                    Restored++;
                    if (G.Diag >= 1) {
                        char Db[128];
                        sprintf_s(Db, "%s restored net %d (broker) / %d open (saved)",
                            Rows[r].Sym, BQty[B], Rows[r].Cnt);
                        showMsg("posstate restore:", Db);
                    }
                } else {
                    // broker flat / row absent: broker is ground truth - flat
                    // means closed (manually or auto-squared while Zorro was
                    // down). Restore nothing; the map stays empty for this
                    // symbol. v0.23: cancel any orphan SL-M backstop still
                    // working at the exchange (manual close leaves it behind).
                    if (Rows[r].ProtId[0])
                        protCancelId(Rows[r].ProtId, Rows[r].Sym);
                    if (G.Diag >= 1) {
                        char Db[128];
                        sprintf_s(Db, "%s saved %d/%d but broker flat - dropped",
                            Rows[r].Sym, Rows[r].Qty, Rows[r].Cnt);
                        showMsg("posstate restore:", Db);
                    }
                }
            }
            if (NRows || G.Diag >= 1)
                posStateSave(); // rewrite in reconciled form
        }
        return 1;
    } else { // logout
        posStateSave(); // v0.22: persist before the state is torn down
        if (G.HttpId) http_free(G.HttpId);
        G.HttpId = 0;
        *G.Key = 0;
    }
    return 0;
}

DLLFUNC int BrokerAccount(char* Acct, double* pBalance, double* pTradeVal, double* pMarginVal)
{
    if (!isConnected()) return 0;
    char* Response = send("funds", "", 2);
    if (!Response || !statusOK(Response)) return 0;
    // availablecash, collateral, m2mrealized, m2munrealized, utiliseddebits (strings)
    if (pBalance)   *pBalance   = strvar(Response, "availablecash",  0.);
    if (pTradeVal)  *pTradeVal = strvar(Response, "m2munrealized",   0.);
    if (pMarginVal) *pMarginVal= strvar(Response, "utiliseddebits",  0.);
    return 1;
}

// ------- per-quote sanity gate (v0.26) ///////////////////////////////////
// Band gate on every price served to Zorro from the live quotes path.
// The /quotes payload carries the broker's day-truth (open/high/low). A
// price outside [low - tol, high + tol] is treated as a feed artifact
// rather than a market move. tol = 0.5% covers legitimate gaps while
// catching cross-wired or stale-session prices.
static double quoteGatePrice(const char* Symbol, double DayOpen, double DayHigh,
                             double DayLow, double Price)
{
    // locate/claim this symbol's slot
    int Slot = -1, Free = -1;
    for (int i = 0; i < MAX_SYMPOS; i++) {
        if (G.QuoteGate[i].LastGood > 0. && 0 == strcmpi(G.QuoteGate[i].Sym, Symbol)) {
            Slot = i; break;
        }
        if (Free < 0 && G.QuoteGate[i].LastGood == 0.) Free = i;
    }
    if (Slot < 0) {
        if (Free < 0) return Price; // table full: fail open
        Slot = Free;
        strcpy_s(G.QuoteGate[Slot].Sym, Symbol);
    }

    // band check: broker day-truth only counts when plausible itself
    const double tol = 0.005; // 0.5%
    bool BandOK = (DayLow > 0. && DayHigh > 0. && DayLow <= DayHigh);
    bool InBand = !BandOK
        || (Price >= DayLow * (1. - tol) && Price <= DayHigh * (1. + tol));

    if (InBand) {
        G.QuoteGate[Slot].LastGood = Price;
        G.QuoteGate[Slot].Rejects  = 0;
        return Price;
    }
    // out of band: self-heal after 3 consecutive out-of-band polls (the
    // broker's day-high/low may briefly lag a genuine fast move)...
    G.QuoteGate[Slot].Rejects++;
    if (G.QuoteGate[Slot].LastGood > 0. && G.QuoteGate[Slot].Rejects >= 3) {
        // ...but a NEW session-seed (fresh slot, no history) or a persistent
        // move re-anchors: accept and record
        G.QuoteGate[Slot].LastGood = Price;
        if (G.Diag >= 1) showMsg("quote gate re-anchored:", upperSymbol(Symbol));
        return Price;
    }
    // hold the last known-good price for this poll
    double Held = G.QuoteGate[Slot].LastGood;
    if (G.Diag >= 1) {
        char SB[160];
        sprintf_s(SB, "%.2f -> held %.2f (band %.2f..%.2f)",
            Price, Held, DayLow, DayHigh);
        showMsg("quote gate reject:", SB);
    }
    return (Held > 0.) ? Held : Price; // no anchor yet: fail open
}

DLLFUNC int BrokerAsset(char* Symbol, double* pPrice, double* pSpread,
    double* pVolume, double* pPip, double* pPipCost, double* pMinAmount,
    double* pMargin, double* pRollLong, double* pRollShort)
{
    if (G.Diag >= 1) {
        char Db[128];
        sprintf_s(Db, "Symbol=%s KeyLen=%d", Symbol ? Symbol : "(null)", (int)strlen(G.Key));
        showMsg("BrokerAsset:", Db);
    }
    if (!isConnected() || !Symbol) return 0;
    strcpy_s(G.Symbol, Symbol);
    sessionExchAdd(exchangeOf(Symbol)); // v0.28: session clock follows the
                                        // subscribed exchanges (BrokerTime)
    // v0.29 SESSION GATE: outside the subscribed exchanges' trading
    // window, no price is served (return 0 = "market closed" to Zorro).
    // Zorro completes a bar only when a price arrives, so rejecting the
    // poll prevents bar formation outside the session window. Placement
    // BEFORE the network call: no price traffic outside sessions at all.
    // Only price polls (pPrice != NULL) are gated - the subscription
    // call (pPrice == NULL) passes through so an evening Zorro start
    // still subscribes all assets (Error 053 would otherwise disable
    // them until the next restart). Opening auction snap (09:15:00) is
    // excluded by the 15-second offset in sessionOpenNow().
    if (pPrice && !sessionOpenNow()) {
        if (G.Diag >= 1) {
            char Db[96];
            sprintf_s(Db, "%s - session closed, quote refused", upperSymbol(Symbol));
            showMsg("session gate:", Db);
        }
        return 0; // Zorro reads this as market-closed: no bar, no trade
    }
    char Inner[256];
    sprintf_s(Inner, "\"symbol\":\"%s\",\"exchange\":\"%s\"",
        apiSymbolOf(Symbol), exchangeOf(Symbol));
    char* Response = send("quotes", Inner, 2);
    // Kotak/OpenAlgo intermittently returns {"status":"error","message":
    // "Failed to fetch quotes"} (upstream hiccup) and openalgo occasionally
    // drops transfers. Both look identical to Zorro if we give up: the first
    // asset's quote failure at session start makes Zorro declare
    // "Market closed" and end the session. Retry both failure classes
    // 3x (~2s total) before reporting failure to Zorro.
    for (int Retry = 0; Retry < 3 && (!Response || !statusOK(Response)); Retry++) {
        sleep(650);
        Response = send("quotes", Inner, 2);
    }
    if (!Response || !statusOK(Response)) {
        // "market closed" rejections are normal outside session hours -
        // log them quietly; everything else is an error worth showing.
        char* Msg = Response ? strtext(Response, "message", "") : "";
        if (!Response || !strstr(Msg, "market") || strstr(Msg, "Failed to fetch"))
            failLog("quotes call failed:", Response);
        return 0;
    }

    double Ltp = strvar(Response, "ltp", 0.);
    double Ask = strvar(Response, "ask", 0.);
    double Bid = strvar(Response, "bid", 0.);
    double Vol = strvar(Response, "volume", 0.);
    double dOpen  = strvar(Response, "open",  0.);  // broker day-truth (v0.26)
    double dHigh  = strvar(Response, "high",  0.);  // band for the quote gate
    double dLow   = strvar(Response, "low",   0.);
    if (Ask <= 0.) Ask = Ltp;
    if (Bid <= 0.) Bid = Ltp;
    if (Ask <= 0. || Ltp == 0.) {
        failLog("quotes parse failed:", Response);
        return 0; // symbol unavailable -> Error 053
    }
    // v0.26: gate the price served to Zorro against the day's broker-truth
    // band. A feed artifact (transient connectivity issue, stale session)
    // is held at the last known-good price instead of building a distorted
    // bar.
    Ask = quoteGatePrice(Symbol, dOpen, dHigh, dLow, Ask);
    if (Ask <= 0.) {
        failLog("quotes gate rejected:", Response);
        return 0;
    }
    if (pPrice)   *pPrice  = Ask;
    if (pSpread)  *pSpread = (Ask - Bid > 0.) ? (Ask - Bid) : 0.;
    if (pVolume)  *pVolume = Vol;

    // India EQ statics, overridable from the CSV map
    ASSETCFG* A = findAsset(Symbol);
    double Pip = A ? A->PipVal : 0.05;
    double Lot = A ? A->LotAmt : 1.;
    double PCost = A ? A->PipCostVal : Pip * Lot;
    double Mgn = A ? A->MgnCost : -5.;
    if (pPip)        *pPip = Pip;
    if (pPipCost)    *pPipCost = PCost;
    if (pMinAmount)  *pMinAmount = Lot;
    if (pMargin)     *pMargin = Mgn;
    if (pRollLong)   *pRollLong = 0.;
    if (pRollShort)  *pRollShort = 0.;
    return 1;
}

DLLFUNC int BrokerHistory2(char* Symbol, DATE Start, DATE End, int TickMinutes, int NTicks, T6* Ticks)
{
    if (!Ticks || !NTicks || !Symbol) return 0;
    // Zorro sessions can carry a future EndDate; brokers serve no future
    // candles, so clamp End to tomorrow-midnight UTC (keeps today's bars).
    SYSTEMTIME stNow;
    GetSystemTime(&stNow);
    double vTomorrow = 0.;
    if (SystemTimeToVariantTime(&stNow, &vTomorrow)) {
        vTomorrow += 1.;
        if (End > vTomorrow) End = vTomorrow;
    }
    // map TickMinutes -> interval (OpenAlgo: 1m 3m 5m 10m 15m 30m 1h D W)
    const char* Interval = "5m";
    DATE TickSpan = 5. / 1440;
    if (1440 <= TickMinutes)      { Interval = "D";  TickSpan = 1.; }
    else if (60 == TickMinutes)   { Interval = "1h"; TickSpan = 1. / 24; }
    else if (1 == TickMinutes)    { Interval = "1m"; TickSpan = 1. / 1440; }
    else if (3 == TickMinutes)    { Interval = "3m"; TickSpan = 3. / 1440; }
    else if (10 == TickMinutes)   { Interval = "10m"; TickSpan = 10. / 1440; }
    else if (15 == TickMinutes)   { Interval = "15m"; TickSpan = 15. / 1440; }
    else if (30 == TickMinutes)   { Interval = "30m"; TickSpan = 30. / 1440; }
    else if (5 == TickMinutes)    { Interval = "5m"; TickSpan = 5. / 1440; }
    else                          { Interval = "5m"; TickSpan = (double)TickMinutes / 1440.; }

    char SDate[16], EDate[16];
    dateToISO(End, EDate, sizeof(EDate));
    dateToISO(End - NTicks * TickSpan, SDate, sizeof(SDate));

    char Inner[512];
    sprintf_s(Inner, "\"symbol\":\"%s\",\"exchange\":\"%s\",\"interval\":\"%s\",\"start_date\":\"%s\",\"end_date\":\"%s\"",
        apiSymbolOf(Symbol), exchangeOf(Symbol), Interval, SDate, EDate);

    // v0.30: LOCAL .t6 FIRST for the standard 5m interval, REST only fills
    // the tail (today's prices). The .t6 files are refreshed daily and
    // verified bar-for-bar against the API: they are the reference source.
    // This ordering keeps the lookback anchored to verified data; REST
    // candles are additionally filtered to stamps NEWER than the .t6 head,
    // so mis-stamped REST responses cannot enter the series, and a REST
    // serve whose newest candle is older than 3.5 days is discarded
    // outright.
    int Received = 0;
    DATE T6Newest = 0.;
    const int T6First = (5 == TickMinutes);
    if (T6First) {
        Received = readT6History(Symbol, Interval, Start, End, NTicks, Ticks);
        if (Received > 0) T6Newest = Ticks[Received - 1].time; // chronological: last = newest
    }

    // REST pass: parse into a heap buffer, then guard and merge.
    char* Response = send("history", Inner);

    // OpenAlgo /history is only populated when the broker plugin exposes
    // historical data (verified: some brokers return an empty data array).
    int M = 0;              // accepted REST candles (chronological after reverse)
    int DroppedOld = 0;     // candles rejected by the newer-than-.t6-head filter
    T6* Temp = (T6*)malloc((size_t)NTicks * sizeof(T6));
    if (Response && statusOK(Response) && Temp) {
        char* Pos = strchr(Response, '[');
        if (!Pos) Pos = Response;
        while (M < NTicks) {
            char* Open = strrchr(Pos, '{'); // walk backwards (most-recent first)
            if (!Open) break;
            char* TStr = strtext(Open, "timestamp", "");
            if (!*TStr) { *Open = 0; continue; }
            DATE t = 0;
            if (!parseTimestamp(TStr, &t)) { *Open = 0; continue; }
            t += TickSpan; // bar close time per Zorro convention
            if (t < Start) break;
            if (t > End) { *Open = 0; continue; }
            // v0.30 wrong-era guard: keep only candles NEWER than the .t6 head
            if (T6Newest > 0. && t <= T6Newest) { DroppedOld++; *Open = 0; continue; }
            // v0.20: skip dead bars (O==H==L==C, zero range). Zero-range
            // bars (exchange closing prints, index/closing-auction freezes)
            // carry no tradable information and distort range-based
            // indicators such as ATR.
            double dOpen  = strvar(Open, "open",   0.);
            double dHigh  = strvar(Open, "high",   0.);
            double dLow   = strvar(Open, "low",    0.);
            double dClose = strvar(Open, "close",  0.);
            if (dOpen == dHigh && dHigh == dLow && dLow == dClose) {
                *Open = 0; // strip dead candle, do not fill
                continue;
            }
            Temp[M].time   = t;
            Temp[M].fOpen  = dOpen;
            Temp[M].fHigh  = dHigh;
            Temp[M].fLow   = dLow;
            Temp[M].fClose = dClose;
            Temp[M].fVol   = strvar(Open, "volume", 0.);
            M++;
            *Open = 0; // strip processed candle
        }
        // v0.19/v0.30: reverse in place -> chronological order for Zorro's
        // live-history loader (same bug as the .t6 fallback path above).
        for (int a = 0, b = M - 1; a < b; a++, b--) {
            T6 Tmp = Temp[a]; Temp[a] = Temp[b]; Temp[b] = Tmp;
        }
        // v0.30 staleness guard: a REST serve whose newest candle is far in
        // the past (e.g. year-old candles from a degraded API response) is
        // discarded outright instead of being re-stamped by Zorro into the
        // current window.
        if (M > 0) {
            double vNow = 0.;
            SYSTEMTIME stN;
            GetSystemTime(&stN);
            if (SystemTimeToVariantTime(&stN, &vNow) && Temp[M - 1].time < vNow - 3.5) {
                if (G.Diag >= 1)
                    showMsg("REST history stale (newest >3.5d old) - discarded:", i64toa((__int64)M));
                M = 0;
            }
        }
        if (G.Diag >= 1 && M > 0) {
            SYSTEMTIME sA, sB;
            VariantTimeToSystemTime(Temp[0].time, &sA);
            VariantTimeToSystemTime(Temp[M - 1].time, &sB);
            char Diag[192];
            sprintf_s(Diag, "REST tail bars: %d  %04d-%02d-%02d %02d:%02d .. %04d-%02d-%02d %02d:%02d (chronological)",
                M, sA.wYear, sA.wMonth, sA.wDay, sA.wHour, sA.wMinute,
                sB.wYear, sB.wMonth, sB.wDay, sB.wHour, sB.wMinute);
            showMsg(Diag, "");
        }
        if (G.Diag >= 1 && M == 0 && T6Newest > 0. && DroppedOld > 0)
            showMsg("REST candles all older than .t6 head - dropped (wrong-era guard):", i64toa((__int64)DroppedOld));
    }

    // ---- merge: .t6 bars (older) first, REST bars (newer) appended; the
    // newest NTicks bars overall are kept ----
    if (M > 0) {
        if (T6First && Received > 0) {
            int Keep = NTicks - M;
            if (Keep < 0) Keep = 0;
            if (Keep > Received) Keep = Received;
            if (Keep < Received) {
                // drop the oldest .t6 bars, move the newest Keep to the front
                for (int i = 0; i < Keep; i++) Ticks[i] = Ticks[Received - Keep + i];
            }
            Received = Keep;
        } else {
            // no .t6 source (or non-5m interval): REST is the sole series.
            // Keep only the newest NTicks REST candles if the response was larger.
            if (M > NTicks) {
                for (int i = 0; i < NTicks; i++) Temp[i] = Temp[M - NTicks + i];
                M = NTicks;
            }
            Received = 0;
        }
        for (int i = 0; i < M; i++) Ticks[Received++] = Temp[i];
    }
    if (Temp) free(Temp);

    // Fall back to the local frozen .t6 file when no REST bars survived and
    // the .t6 was not already the primary source. readT6History tries the
    // requested interval suffix first, then "5m".
    if (Received <= 0 && !T6First) {
        Received = readT6History(Symbol, Interval, Start, End, NTicks, Ticks);
        if (G.Diag >= 1)
            showMsg("t6 fallback bars:", i64toa((__int64)Received));
    }
    return Received;
}

// returns NAY when no position; NAY-1 when closed; else the TRADE's remaining
// quantity (per-slot registry; v0.16 - was symbol-level net, see plan note)
// v0.16: reports the TRADE's own remaining quantity (from the per-slot
// registry) instead of the symbol-level positionbook net, so each
// half-trade reports only its own share.
// v0.18: per-trade qty = mapped position / open-trade count (equal halves by
// design; Zorro passes its OWN ids here which cannot be mapped to slots).
DLLFUNC int BrokerTrade(int nTradeID, double* pOpen, double* pClose, double* pCost, double* pProfit)
{
    if (!isConnected() || !*G.Symbol) return 0;
    char* Response = send("positionbook", "", 2);
    if (!Response || !statusOK(Response)) {
        // v0.22: a failed positionbook call is a NETWORK error, not a flat
        // position. Returning NAY would tell Zorro "no position / trades
        // gone" while the broker may still hold real positions. Instead:
        // report the trade ALIVE with last-known map quantities.
        // pClose/pOpen stay untouched -> Zorro estimates from its own prices.
        int OwnQty2  = symPosGet(G.Symbol);
        int OpenCnt2 = 0;
        for (int i = 0; i < MAX_SYMPOS; i++)
            if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, G.Symbol)) {
                OpenCnt2 = G.SymPos[i].Cnt; break;
            }
        if (G.Diag >= 1)
            showMsg("BrokerTrade: positionbook failed - outage? reporting last-known state", "");
        if (OpenCnt2 > 0 && OwnQty2 != 0) {
            int PerTradeOut = abs(OwnQty2 / OpenCnt2);
            if (PerTradeOut == 0) PerTradeOut = abs(OpenCnt2);
            return PerTradeOut;
        }
        return NAY; // genuinely nothing mapped: keep the NAY contract
    }
    char Want[64];
    strcpy_s(Want, apiSymbolOf(G.Symbol));
    char* Scan = Response;
    int SymQty = 0; int SymFound = 0;
    double RowLtp = 0., RowAvg = 0., RowPnl = 0.;
    while (Scan) {
        char* Row = strchr(Scan, '{');
        if (!Row) break;
        char* Sym = strtext(Row, "symbol", "");
        double Qty   = strvar(Row, "quantity", 0.);
        double Pnl   = strvar(Row, "pnl", 0.);
        double Ltp   = strvar(Row, "ltp", 0.);
        double Avg   = strvar(Row, "average_price", 0.);
        Scan = strchr(Row + 1, '{');
        if (0 != strcmpi(Sym, Want)) continue;
        SymQty = (int)Qty; SymFound = 1;
        RowLtp = Ltp; RowAvg = Avg; RowPnl = Pnl;
        break;
    }
    // per-trade truth: mapped position / open-trade count
    int OwnQty  = symPosGet(G.Symbol);
    int OpenCnt = 0;
    for (int i = 0; i < MAX_SYMPOS; i++)
        if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, G.Symbol)) {
            OpenCnt = G.SymPos[i].Cnt; break;
        }
    // v0.21: the map position sign must AGREE with the broker position
    // sign; otherwise Zorro would manage a position that does not exist
    // (or miss one that does). The broker positionbook is the ground
    // truth for the NET position; the map only refines the per-trade
    // split of that net. On a sign mismatch, sync the map to the broker.
    if (SymFound && OpenCnt > 0 && OwnQty != 0) {
        int BQty = SymQty;  // signed broker net
        if ((OwnQty > 0 && BQty < 0) || (OwnQty < 0 && BQty > 0)) {
            if (G.Diag >= 1) {
                char Db[160];
                sprintf_s(Db, "map %d vs broker %d sign mismatch - syncing to broker",
                    OwnQty, BQty);
                showMsg("BrokerTrade:", Db);
            }
            // resync: overwrite map qty, keep the open-trade count split
            for (int i = 0; i < MAX_SYMPOS; i++)
                if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, G.Symbol)) {
                    G.SymPos[i].Qty = BQty;
                    break;
                }
            OwnQty = BQty;
        }
        // additional trap: |map| may exceed the broker net (partial untracked
        // close). Cap the map at the broker net so closes can never exceed it.
        if ((abs(OwnQty) > abs(SymQty)) && SymQty != 0) {
            for (int i = 0; i < MAX_SYMPOS; i++)
                if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, G.Symbol)) {
                    G.SymPos[i].Qty = SymQty; // cap at broker net, keep sign
                    break;
                }
            OwnQty = SymQty;
        }
    }
    int PerTrade = (OpenCnt > 0) ? OwnQty / OpenCnt : 0;
    if (G.Diag >= 1) {
        char Db[128];
        sprintf_s(Db, "trade %d own=%d open=%d perTrade=%d broker_sym=%d",
            nTradeID, OwnQty, OpenCnt, PerTrade, SymQty);
        showMsg("BrokerTrade:", Db);
    }
    // closed detection: nothing mapped (per-trade share exhausted) OR the
    // broker reports the symbol flat. The SymQty==0 -> NAY-1 return is
    // GATED on the fill-lag window: a fresh entry takes ~10s to appear in
    // the broker positionbook (order 03:50:03, fill 03:50:13). During
    // that window the broker shows flat; reporting "trade closed" there
    // would close a live trade prematurely. If an order was placed in the
    // last 30s, report the trade ALIVE with last-known quantities instead.
    int BrokerFlat = (SymFound && SymQty == 0) || !SymFound;
    if (BrokerFlat && OpenCnt > 0) {
        int RecentOrder = (GetTickCount() - G.LastOrderTick) < 30000
                          && G.LastOrderTick != 0;
        if (RecentOrder) {
            // fill-lag window: keep the trade alive with last-known per-trade
            if (G.Diag >= 1)
                showMsg("BrokerTrade: broker flat but order <30s old - fill lag, trade alive", "");
            if (pClose && RowLtp > 0.) *pClose = RowLtp;
            int PerTradeLag = OwnQty / OpenCnt; // sign-correct, may be negative
            if (PerTradeLag == 0) PerTradeLag = (OwnQty < 0) ? -1 : 1;
            return abs(PerTradeLag);
        }
        // genuinely flat for >30s after the last order: reset the stale map
        // row (manual close, broker auto-square, MIS square-off) - a stale
        // map would mis-classify the NEXT entry as a close and corrupt state.
        for (int i = 0; i < MAX_SYMPOS; i++)
            if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, G.Symbol)) {
                if (G.Diag >= 1) {
                    char Db[160];
                    sprintf_s(Db, "broker flat%s but map %d/%d open - resetting map row",
                        SymFound ? "" : " (no positionbook row)",
                        G.SymPos[i].Qty, G.SymPos[i].Cnt);
                    showMsg("BrokerTrade:", Db);
                }
                // v0.23: the position is gone at the broker - cancel any
                // orphan SL-M backstop still working at the exchange
                if (G.SymPos[i].ProtId[0]) protCancel(i);
                G.SymPos[i].Qty = 0;
                G.SymPos[i].Cnt = 0;
                posStateSave();
                break;
            }
        return NAY - 1;
    }
    if (PerTrade == 0 || (SymFound && SymQty == 0)) return NAY - 1;
    // Row-level fields: pClose/pOpen only when sane; Zorro estimates whatever
    // we leave untouched.
    if (pClose && RowLtp > 0.)  *pClose = RowLtp;
    if (pOpen  && RowAvg > 0.)  *pOpen  = RowAvg;
    if (pProfit && RowPnl != 0.) *pProfit = RowPnl;
    return abs(PerTrade);
}

// Last-trade price of an asset from /quotes, 0 on failure.
// Used as a sanity anchor for reported fill prices.
double ltpOf(const char* Symbol)
{
    if (!Symbol || !*Symbol) return 0.;
    char Inner[256];
    sprintf_s(Inner, "\"symbol\":\"%s\",\"exchange\":\"%s\"",
        apiSymbolOf(Symbol), exchangeOf(Symbol));
    char* Response = send("quotes", Inner, 2);
    if (!Response || !statusOK(Response)) return 0.;
    return strvar(Response, "ltp", 0.);
}

// ------- SL-M protective backstop (v0.23) ///////////////////////////////
// ONE SL-M order per symbol, script-published, mirroring the TIGHTEST
// open-trade stop level over the full open quantity: an exchange-resident
// disaster backstop for wifi / OpenAlgo-app / PC outages (Zorro's own
// per-tick stops stay primary). Lifecycle:
//   - command 2020 (SET_PROTSTOP): place/replace/cancel on publish
//   - BrokerSell2: cancel the backstop BEFORE closing through Zorro so the
//     two stop layers can never double-fire
//   - "fired while away" auto-detection via the orderbook status
//   - persisted per symbol, restored + re-verified at login

// is a protective order ALIVE at the broker? 1 = open/pending (a live
// stop-loss); 0 = everything else (complete, cancelled, REJECTED, expired,
// or not found). Rejected orders are the common case after an RMS block -
// they are dead and must never block a retry.
int protIsAlive(const char* Oid)
{
    if (!Oid || !*Oid) return 0;
    char* Response = send("orderbook", "", 2);
    if (!Response || !statusOK(Response)) return 0;
    char* Scan = Response;
    while (Scan) {
        char* Row = strchr(Scan, '{');
        if (!Row) break;
        Scan = strchr(Row + 1, '{');
        char* Id = strtext(Row, "orderid", "");
        if (!*Id || 0 != strcmp(Id, Oid)) continue;
        char* St = strtext(Row, "order_status", "");
        return (0 == strcmpi(St, "open")
             || 0 == strcmpi(St, "pending")
             || 0 == strcmpi(St, "trigger pending"));
    }
    return 0; // not found -> dead
}

// scan the orderbook for an order's status: "complete"/"completed" -> 1
int orderIsComplete(const char* Oid)
{
    if (!Oid || !*Oid) return 0;
    char* Response = send("orderbook", "", 2);
    if (!Response || !statusOK(Response)) return 0;
    char* Scan = Response;
    while (Scan) {
        char* Row = strchr(Scan, '{');
        if (!Row) break;
        Scan = strchr(Row + 1, '{');
        char* Id = strtext(Row, "orderid", "");
        if (!*Id || 0 != strcmp(Id, Oid)) continue;
        char* St = strtext(Row, "order_status", "");
        return (0 == strcmpi(St, "complete") || 0 == strcmpi(St, "completed"));
    }
    return 0; // not found, open or rejected-by-exchange -> not complete
}

// find a symbol's map row; -1 when absent
int symPosRow(const char* Sym)
{
    if (!Sym || !*Sym) return -1;
    for (int i = 0; i < MAX_SYMPOS; i++)
        if (G.SymPos[i].Sym[0] && 0 == strcmpi(G.SymPos[i].Sym, Sym))
            return i;
    return -1;
}

// find or create a symbol's map row; -1 when the table is full
int symPosCreate(const char* Sym)
{
    int Row = symPosRow(Sym);
    if (Row >= 0) return Row;
    for (int i = 0; i < MAX_SYMPOS; i++) {
        if (!G.SymPos[i].Sym[0]) {
            strcpy_s(G.SymPos[i].Sym, upperSymbol(Sym));
            G.SymPos[i].Qty = 0;
            G.SymPos[i].Cnt = 0;
            G.SymPos[i].ProtId[0] = 0;
            G.SymPos[i].ProtLevel = 0.;
            G.SymPos[i].ProtQty = 0;
            return i;
        }
    }
    return -1;
}

// cancel a symbol's SL-M backstop (clears its row fields + persists).
// Returns 1 = cancelled (or none existed); 0 = order could NOT be cancelled
// (transient failure with the id kept for retry, or it ALREADY FIRED -
// caller must not close/replace: the broker position changed under us).
int protCancel(int Row)
{
    if (Row < 0 || !G.SymPos[Row].ProtId[0]) return 1;
    char Oid[64];
    strcpy_s(Oid, G.SymPos[Row].ProtId);
    char Sym[32];
    strcpy_s(Sym, G.SymPos[Row].Sym);
    int Ok = protCancelId(Oid, Sym);
    if (Ok) {
        G.SymPos[Row].ProtId[0] = 0;
        G.SymPos[Row].ProtLevel = 0.;
        G.SymPos[Row].ProtQty = 0;
        posStateSaveSym(Sym);
    }
    return Ok;
}

// generic cancel by order id: 1 = cancelled/none, 0 = fired or failed
int protCancelId(const char* Oid, const char* Sym)
{
    if (!Oid || !*Oid) return 1;
    char Inner[256];
    sprintf_s(Inner, "\"orderid\":\"%s\",\"strategy\":\"Zorro\"", Oid);
    char* Response = send("cancelorder", Inner, 2);
    if (Response && statusOK(Response)) {
        if (G.Diag >= 1) showMsg("SL-M cancelled:", Sym);
        return 1;
    }
    if (!protIsAlive(Oid)) {
        // REJECTED (e.g. RMS rejection with no real position), cancelled or
        // expired at the broker: dead - release it. If it actually FIRED,
        // say so: the position it protected is (partly) closed at the broker
        // and the map resyncs from the positionbook on the next poll.
        if (orderIsComplete(Oid))
            showMsg("PROTECTIVE SL-M FIRED at broker:", Sym);
        else if (G.Diag >= 1)
            showMsg("SL-M dead at broker (released):", Oid);
        return 1;
    }
    failLog("SL-M cancel failed (kept for retry):", Oid);
    return 0;
}

// place the symbol's SL-M backstop. SignedQty > 0 = SELL SL-M (protects a
// long), < 0 = BUY SL-M (protects a short). Skips without error when the
// trigger sits on the wrong side of the LTP (Kotak rejects those; the next
// bar's publish retries once the price has moved).
void protPlace(const char* Sym, double Level, int SignedQty)
{
    if (!Sym || !*Sym || Level <= 0. || SignedQty == 0) return;
    int Row = symPosCreate(Sym);
    if (Row < 0) { showMsg("SL-M no map slot:", upperSymbol(Sym)); return; }
    double Ltp = ltpOf(Sym);
    if (Ltp <= 0.) { showMsg("SL-M skipped (no LTP):", upperSymbol(Sym)); return; }
    // trigger sanity: a SELL SL sits below the LTP, a BUY SL above; closer
    // than 0.1% to the LTP risks an immediate trigger - skip, retry later
    if (SignedQty > 0 && Level > Ltp * 0.999) {
        if (G.Diag >= 1) showMsg("SL-M skipped (SELL trigger not below LTP):", upperSymbol(Sym));
        return;
    }
    if (SignedQty < 0 && Level < Ltp * 1.001) {
        if (G.Diag >= 1) showMsg("SL-M skipped (BUY trigger not above LTP):", upperSymbol(Sym));
        return;
    }
    char Inner[512];
    sprintf_s(Inner, "\"strategy\":\"Zorro\",\"symbol\":\"%s\",\"exchange\":\"%s\","
        "\"action\":\"%s\",\"pricetype\":\"SL-M\",\"product\":\"%s\",\"quantity\":\"%d\","
        "\"trigger_price\":\"%.2f\"",
        apiSymbolOf(Sym), exchangeOf(Sym),
        (SignedQty > 0) ? "SELL" : "BUY", productOf(Sym),
        (SignedQty > 0) ? SignedQty : -SignedQty, Level);
    char* Response = send("placeorder", Inner);
    char* Oid = Response ? strtext(Response, "orderid", "") : "";
    if (!*Oid && Response) Oid = strtext(Response, "order_id", "");
    if (!Response || !statusOK(Response) || !*Oid) {
        failLog("SL-M place failed:", Response ? strtext(Response, "message", "") : "(no response)");
        return;
    }
    strcpy_s(G.SymPos[Row].ProtId, Oid);
    G.SymPos[Row].ProtLevel = Level;
    G.SymPos[Row].ProtQty = SignedQty;
    posStateSaveSym(Sym);
    char Db[128];
    sprintf_s(Db, "%s %s %d @ trigger %.2f (id %s)", upperSymbol(Sym),
        (SignedQty > 0) ? "SELL" : "BUY", (SignedQty > 0) ? SignedQty : -SignedQty,
        Level, Oid);
    showMsg("SL-M placed:", Db);
}

// Average fill price of a (completed) order. Primary: /orderbook "price"
// once order_status == "complete"; fallback: /tradebook "average_price".
// Polls up to ~8 s. Returns 0 when unknown.
double fillPriceOf(const char* OrderId)
{
    if (!OrderId || !*OrderId) return 0.;
    for (int attempt = 0; attempt < 16; attempt++) {
        if (attempt) sleep(500);
        char* Response = send("orderbook", "", 2);
        if (Response && statusOK(Response)) {
            char* Scan = Response;
            while (Scan) {
                char* Row = strchr(Scan, '{');
                if (!Row) break;
                Scan = strchr(Row + 1, '{');
                char* Oid = strtext(Row, "orderid", "");
                if (!*Oid || 0 != strcmp(Oid, OrderId)) continue;
                char* St = strtext(Row, "order_status", "");
                double Price = strvar(Row, "price", 0.);
                if (Price > 0. && (0 == strcmpi(St, "complete") || 0 == strcmpi(St, "completed")))
                    return Price;
                break; // right row but not (yet) complete -> keep polling
            }
        }
        // halfway: try the tradebook - fills appear there as they execute
        if (attempt == 7) {
            char* Response = send("tradebook", "", 2);
            if (Response && statusOK(Response)) {
                char* Scan = Response;
                while (Scan) {
                    char* Row = strchr(Scan, '{');
                    if (!Row) break;
                    Scan = strchr(Row + 1, '{');
                    char* Oid = strtext(Row, "orderid", "");
                    if (!*Oid || 0 != strcmp(Oid, OrderId)) continue;
                    double Avg = strvar(Row, "average_price", 0.);
                    if (Avg > 0.) return Avg;
                }
            }
        }
    }
    return 0.;
}

// place a market/limit order. Vol>0 => BUY, <0 => SELL.
static int placeOrder(char* Symbol, int Vol, double Limit, double* pPrice, int* pFill)
{
    if (!isConnected() || !Vol || !Symbol) return 0;
    // armed option contract? trade the composed class symbol instead of the
    // (short, non-resolvable) Zorro asset symbol; disarms after one order so
    // underlying orders are never hijacked by a stale arm.
    char OptSym[64];
    if (G.OptStrike[0] && G.OptType[0]) {
        strcpy_s(OptSym, optClassSymbol());
        G.OptStrike[0] = 0;             // one-shot: disarm immediately
        Symbol = OptSym;
    }
    char Inner[1024];
    char LimitPart[64];
    *LimitPart = 0;
    if (Limit > 0.)
        sprintf_s(LimitPart, ",\"price\":\"%.2f\"", Limit);  // plain LIMIT: no trigger_price (Setting trigger=price wrongly creates SL-Limit)
    sprintf_s(Inner, "\"strategy\":\"Zorro\",\"symbol\":\"%s\",\"exchange\":\"%s\","
        "\"action\":\"%s\",\"pricetype\":\"%s\",\"product\":\"%s\",\"quantity\":\"%d\"%s",
        upperSymbol(Symbol), exchangeOf(Symbol),
        (Vol > 0) ? "BUY" : "SELL",
        (Limit > 0.) ? "LIMIT" : "MARKET",
        productOf(Symbol),
        (int)labs(Vol),
        LimitPart);

    char* Response = send("placeorder", Inner);
    if (!Response || !statusOK(Response)) {
        if (Response) showMsg("Order rejected:", strtext(Response, "message", ""));
        return 0;
    }
    G.LastOrderTick = GetTickCount(); // fill-lag gate anchor (v0.21)
    // own-position map: track what THIS plugin placed (sign-aware); the
    // clamp layer in BrokerSell2 leans on this to prevent overselling.
    // Closes are tracked too (signed Vol subtracts) - exactly once; the
    // caller (BrokerSell2) does NOT subtract again.
    symPosAdd(Symbol, Vol);
    posStateSave(); // v0.22: persist after every tracked order (survives relogin)
    if (G.Diag >= 1) {
        char Db[128];
        sprintf_s(Db, "requested=%d submitted=%d %s %s",
            Vol, Vol, (Vol > 0) ? "BUY" : "SELL", upperSymbol(Symbol));
        showMsg("Order qty:", Db);
    }
    char* Oid = strtext(Response, "orderid", "");
    if (!*Oid) Oid = strtext(Response, "order_id", "");
    if (!*Oid) return 0;
    strcpy_s(G.Uuid, Oid);
    // Kotak order ids are 15-digit (e.g. 260909000359055) and overflow the
    // 32-bit int Zorro uses for trade ids. Keep a small side table
    // (slot id <-> real order id) and hand Zorro the slot as trade id.
    // DO_CANCEL / GET_UUID resolve through the table; the plugin's own
    // G.Uuid still holds the raw Kotak id for direct use.
    {
        int Slot = 0; int i;
        for (i = 0; i < 10; i++)
            if (!G.TradeIds[i][0] || 0 == strcmp(G.TradeIds[i], Oid)) { Slot = i; break; }
        if (i >= 10) Slot = G.IdxNext++ % 10; // overwrite oldest
        strcpy_s(G.TradeIds[Slot], Oid);
        G.LastSlot = Slot + 1;   // positive pseudo-id (6-digit range)
    }
    if (pFill)  *pFill  = (int)labs(Vol); // assume full fill; refined by BrokerTrade
    if (pPrice) {
        // Entry price for Zorro's book. Zorro 3.016 books a literal 0/partial
        // value if we hand back 0 (live evidence 09-09..09-11: entries booked
        // as 0.0/0.10/0.30/83.10 leading to instant bogus stop-outs). So
        // ALWAYS give a sane price: polled fill, but ONLY if it matches the
        // live LTP (rejects junk from cached/stale broker rows, e.g. a manual
        // option price of 83.10); otherwise the current LTP itself.
        // NB: local names must avoid lite-C g-> macros (Fill, Margin, ...).
        double FillPx = (Limit > 0.) ? Limit : fillPriceOf(Oid);
        double LtpPx  = ltpOf(Symbol);
        if (FillPx > 0. && LtpPx > 0. && (FillPx > LtpPx * 1.02 || FillPx < LtpPx * 0.98)) {
            if (G.Diag >= 1) {
                char SB[64];
                sprintf_s(SB, "%.2f (ltp %.2f)", FillPx, LtpPx);
                showMsg("fill price rejected (sanity):", SB);
            }
            FillPx = 0.;
        }
        if (FillPx <= 0. && LtpPx > 0.)
            FillPx = (Limit > 0.) ? Limit : LtpPx;
        if (FillPx <= 0.) FillPx = Limit;   // last resort: limit or 0
        *pPrice = FillPx;
        if (G.Diag >= 1) {
            char FB[96];
            sprintf_s(FB, "%.2f", *pPrice);
            showMsg("Fill price:", FB);
        }
    }
    return -1; // signal GET_UUID: Zorro fetches our pseudo id via G.LastSlot
}

DLLFUNC int BrokerBuy2(char* Symbol, int Vol, double StopDist, double Limit, double* pPrice, int* pFill)
{
    return placeOrder(Symbol, Vol, Limit, pPrice, pFill);
}

DLLFUNC int BrokerSell2(int nTradeID, int nVol, double Limit, double* pClose, double* pCost, double* pProfit, int* pFill)
{
    if (!isConnected() || !*G.Symbol || !nVol) return 0;
    // Reverse: closing a long => SELL, closing a short => BUY
    int Vol = -nVol;
    // v0.23: cancel the exchange-side SL-M backstop BEFORE closing through
    // Zorro, so the two stop layers can never both fire. If the backstop
    // already fired (price gapped through the stop), the broker position
    // is closed/reduced: skip the market close - it would create an
    // unintended reverse position - and let the map resync from the
    // positionbook.
    {
        int PRow = symPosRow(G.Symbol);
        if (PRow >= 0 && G.SymPos[PRow].ProtId[0]) {
            if (!protCancel(PRow)) {
                showMsg("close skipped (SL-M fired at broker):", upperSymbol(G.Symbol));
                if (pFill) *pFill = abs(Vol);
                return nTradeID;
            }
        }
    }
    // QUANTITY SAFETY NET (v0.18): Zorro passes ITS OWN trade id here (not
    // our pseudo slots), so per-trade registries can't key on it.
    // Protection is per-SYMBOL instead: clamp the close to this trade's
    // per-trade share of the mapped position (equal halves by design);
    // suppress entirely once nothing is left. This prevents duplicated
    // exits and redundant closes at the plugin.
    int Clamped = clampClose(G.Symbol, Vol);
    if (Clamped == 0) {
        // nothing of this symbol left to close: register the close as done
        // instead of submitting a redundant order to the broker, which
        // would create unintended exposure
        showMsg("close suppressed (nothing left to close):", upperSymbol(G.Symbol));
        // v0.23: nothing left at our map - cancel any orphan SL-M backstop
        // (e.g. a manual close left it working at the exchange)
        int ORow = symPosRow(G.Symbol);
        if (ORow >= 0 && G.SymPos[ORow].ProtId[0]) protCancel(ORow);
        if (pFill) *pFill = 0;
        return nTradeID;
    }
    if (abs(Clamped) < abs(Vol))
        showMsg("close clamped to per-trade share:", upperSymbol(G.Symbol));
    double Price = 0.;
    int Filled = 0;
    if (G.Diag >= 1) {
        char Db[160];
        sprintf_s(Db, "trade %d requested %d submitted %d %s %s",
            nTradeID, abs(Vol), abs(Clamped), (Clamped > 0) ? "BUY" : "SELL", upperSymbol(G.Symbol));
        showMsg("Close order:", Db);
    }
    int Ret = placeOrder(G.Symbol, Clamped, Limit, &Price, &Filled);
    if (!Ret) return 0; // order failed
    if (pFill)   *pFill   = Filled;
    if (pClose)  *pClose  = Price; // 0 -> Zorro estimates from quotes
    if (pCost)   *pCost   = 0.;
    if (pProfit) *pProfit = 0.;    // Zorro books P&L from entry/exit prices
    return nTradeID;               // spec: return the (unchanged) trade ID
}

////////////////////////////////////////////////////////////
DLLFUNC double BrokerCommand(int Mode, intptr_t Parameter)
{
    switch (Mode) {
        case GET_COMPLIANCE:   return 2;     // no NFA restrictions
        case GET_MAXREQUESTS:  return 3;     // OpenAlgo rate, conservative
        case GET_MAXTICKS:     return 5000;  // lookback fill cap (default 300)
        case SET_DIAGNOSTICS:  G.Diag = (int)Parameter; return 1;
        case SET_AMOUNT: {
            G.Unit = *(double*)Parameter;
            return 1;
        }
        case SET_SYMBOL: {
            if (Parameter) strcpy_s(G.Symbol, (const char*)Parameter);
            return 1;
        }
        case SET_OPTCONTRACT: {
            // "strike,YYYYMMDD,CE|PE" from the option-trading script; arms the
            // class-symbol store so the NEXT BrokerBuy2 trades that contract.
            G.OptStrike[0] = G.OptExpiry[0] = G.OptType[0] = 0;
            if (Parameter) {
                const char* Given = (const char*)Parameter;
                const char* C1 = strchr(Given, ',');
                const char* C2 = C1 ? strchr(C1 + 1, ',') : 0;
                if (C1 && C2) {
                    size_t N1 = C1 - Given, N2 = C2 - (C1 + 1);
                    if (N1 > 0 && N1 < 15) { strncpy_s(G.OptStrike, Given, N1); G.OptStrike[N1] = 0; }
                    if (N2 > 0 && N2 < 15) { strncpy_s(G.OptExpiry, C1 + 1, N2); G.OptExpiry[N2] = 0; }
                    char T[4] = { 0,0,0,0 };
                    if (0 == _strnicmp(C2 + 1, "CE", 2)) strcpy_s(G.OptType, "CE");
                    else if (0 == _strnicmp(C2 + 1, "PE", 2)) strcpy_s(G.OptType, "PE");
                    if (G.OptStrike[0] && G.OptExpiry[0] && G.OptType[0]) {
                        char Db[96];
                        sprintf_s(Db, "%s", optClassSymbol());
                        if (G.Diag >= 1) showMsg("OptContract armed:", Db);
                        return 1;
                    }
                }
            }
            showMsg("OptContract BAD (need strike,YYYYMMDD,CE|PE):", Parameter ? (const char*)Parameter : "");
            return 0;
        }
        case GET_UUID:
            strcpy_s((char*)Parameter, 256, G.Uuid); return (var)G.LastSlot;
        case SET_UUID:
            strcpy_s(G.Uuid, (char*)Parameter); return 1;

        case 2020: { // SET_PROTSTOP (v0.24): exchange-side SL-M backstop
            // Parameter (text) = "SYM,trigger,signedqty" (v0.24: symbol
            // carried EXPLICITLY - Zorro batches BrokerAsset calls for all
            // symbols each cycle, so the plugin's internal G.Symbol may be
            // stale when publishes arrive). signedqty > 0 = SELL SL-M
            // protecting a long; < 0 = BUY SL-M protecting a short;
            // qty 0 = cancel the symbol's backstop. Called by the strategy
            // every bar.
            if (!isConnected()) return 0;
            const char* Given = Parameter ? (const char*)Parameter : "";
            char SymS[32] = "", LevelS[32] = "", QtyS[32] = "";
            const char* C1 = Given[0] ? strchr(Given, ',') : 0;
            const char* C2 = C1 ? strchr(C1 + 1, ',') : 0;
            if (C1 && C2) {
                size_t N1 = C1 - Given, N2 = C2 - (C1 + 1);
                if (N1 > 0 && N1 < sizeof(SymS)) {
                    memcpy(SymS, Given, N1); SymS[N1] = 0;
                    if (N2 > 0 && N2 < sizeof(LevelS)) {
                        memcpy(LevelS, C1 + 1, N2); LevelS[N2] = 0;
                        strcpy_s(QtyS, C2 + 1);
                    }
                }
            }
            if (!SymS[0] || !LevelS[0]) {
                // malformed / legacy "trigger,qty" publish: ignore silently
                // except in diag (v0.23 scripts were the old format)
                if (G.Diag >= 1) showMsg("SL-M publish malformed (need SYM,trigger,qty):", Given);
                return 1;
            }
            double Level = atof(LevelS);
            int SignedQty = atoi(QtyS);
            int Row = symPosRow(SymS);
            // cancel-only publish (qty 0): flatten the backstop
            if (Level <= 0. || SignedQty == 0) {
                if (Row >= 0 && G.SymPos[Row].ProtId[0]) return protCancel(Row);
                return 1;
            }
            // v0.23 sanity gate: the published qty must match the map's
            // direction and size (within tolerance). On a flip bar the
            // exiting trade can still be in Zorro's open_trades list while
            // the map already flipped to the new direction - protecting THAT
            // stop would orphan an SL-M on the wrong side. Skip those.
            if (Row >= 0 && G.SymPos[Row].Qty != 0) {
                int SignBad = ((G.SymPos[Row].Qty > 0) != (SignedQty > 0));
                int Diff = SignedQty - G.SymPos[Row].Qty;
                if (Diff < 0) Diff = -Diff;
                int Toler = (G.SymPos[Row].Qty < 0) ? -G.SymPos[Row].Qty / 3 : G.SymPos[Row].Qty / 3;
                if (Toler < 2) Toler = 2;
                if (SignBad || Diff > Toler) {
                    if (G.Diag >= 1) showMsg("SL-M skipped (map/live mismatch):", upperSymbol(SymS));
                    return 1;
                }
            }
            // replace only on a real change (>= half a tick)
            if (Row >= 0 && G.SymPos[Row].ProtId[0]) {
                int Same = (fabs(G.SymPos[Row].ProtLevel - Level) < 0.005
                            && G.SymPos[Row].ProtQty == SignedQty);
                if (Same) return 1; // already in place, nothing to do
                if (!protCancel(Row)) return 0; // fired: broker changed, resync first
            }
            protPlace(SymS, Level, SignedQty);
            return 1;
        }

        case DO_CANCEL: {
            // Parameter is either a raw order id (from SET_UUID) or one of
            // our pseudo slot ids (from BrokerBuy2). Resolve both to the
            // real Kotak orderid string.
            char Real[64]; Real[0] = 0;
            if (Parameter) {
                const char* Given = (const char*)Parameter;
                for (int i = 0; i < 10; i++)
                    if (G.TradeIds[i][0] &&
                        (atoi(Given) == i + 1 || 0 == strcmp(Given, G.TradeIds[i]))) {
                        strcpy_s(Real, G.TradeIds[i]); break;
                    }
                if (!Real[0]) { strcpy_s(Real, Given); }
            }
            if (!Real[0] && G.Uuid[0]) strcpy_s(Real, G.Uuid);
            if (!Real[0]) return 0;
            char Inner[256];
            sprintf_s(Inner, "\"orderid\":\"%s\",\"strategy\":\"Zorro\"", Real);
            char* Response = send("cancelorder", Inner, 2);
            if (Response && statusOK(Response)) return 1;
            return 0;
        }
    }
    return 0.;
}
