// Workscape Phone Engine: motor SIP em processo separado do Electron.
//
// Protocolo: uma mensagem JSON por linha. Comandos entram por stdin; eventos saem por stdout.
// Logs de diagnóstico vão para stderr e NUNCA incluem senhas. A senha SIP chega só pelo comando
// "register" e fica apenas na memória do processo.
//
// Licença: este programa liga o PJSIP (GPL v2 ou licença comercial da Teluu). Uso interno segue a GPL;
// para distribuir ou vender é preciso licença comercial do PJSIP ou trocar de motor (ver docs/LICENCAS.md).
#include <pjsua2.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/json.hpp"

using json = nlohmann::json;
using namespace pj;

namespace {

constexpr const char *kVersion = "0.1.0";
std::mutex g_out_mutex;

// Saída serializada: callbacks do PJSIP chegam em threads diferentes.
void emit(const json &event) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << event.dump() << "\n" << std::flush;
}

void emit_error(const std::string &cmd, const std::string &message, int code = 0) {
    emit({{"event", "error"}, {"cmd", cmd}, {"message", message}, {"code", code}});
}

// Tira do texto qualquer sequência que se pareça com a senha, por segurança extra nos erros devolvidos.
std::string scrub(std::string text, const std::string &secret) {
    if (secret.size() < 4) return text;
    for (size_t pos = text.find(secret); pos != std::string::npos; pos = text.find(secret, pos)) {
        text.replace(pos, secret.size(), "***");
        pos += 3;
    }
    return text;
}

class EngineCall;

// O log do PJSIP vai só para stderr: stdout é reservado ao protocolo JSON (uma mensagem por linha).
class StderrLogWriter : public LogWriter {
public:
    void write(const LogEntry &entry) override {
        std::fputs(entry.msg.c_str(), stderr);
        std::fputc('\n', stderr);
    }
};
StderrLogWriter g_log_writer;

// Toca um arquivo WAV dentro da chamada e avisa quando termina.
class EnginePlayer : public AudioMediaPlayer {
public:
    int callId = -1;
    void onEof2() override { emit({{"event", "play_done"}, {"id", callId}}); }
};

struct Engine {
    Endpoint ep;
    std::unique_ptr<class EngineAccount> account;
    std::vector<std::unique_ptr<EngineCall>> calls;  // dono das chamadas; ids vêm do PJSIP
    std::mutex calls_mutex;
    std::string secret;  // só para limpar mensagens de erro
    std::string domain;  // domínio da conta; destinos de chamada e transferência sempre usam este
    bool nullAudio = false;
    bool started = false;
    std::atomic<bool> dnd{false};  // Não perturbe: chamadas recebidas levam 486 Ocupado na hora, sem tocar
};

Engine g;

std::string call_state_name(pjsip_inv_state s) {
    switch (s) {
        case PJSIP_INV_STATE_NULL: return "null";
        case PJSIP_INV_STATE_CALLING: return "calling";
        case PJSIP_INV_STATE_INCOMING: return "incoming";
        case PJSIP_INV_STATE_EARLY: return "early";
        case PJSIP_INV_STATE_CONNECTING: return "connecting";
        case PJSIP_INV_STATE_CONFIRMED: return "confirmed";
        case PJSIP_INV_STATE_DISCONNECTED: return "disconnected";
    }
    return "unknown";
}

class EngineCall : public Call {
public:
    EngineCall(Account &acc, int id = PJSUA_INVALID_ID) : Call(acc, id) {}
    bool muted = false;
    std::unique_ptr<EnginePlayer> player;

    void onCallState(OnCallStateParam &) override {
        CallInfo ci = getInfo();
        json ev = {{"event", "call_state"},
                   {"id", ci.id},
                   {"state", call_state_name(ci.state)},
                   {"status", (int)ci.lastStatusCode},
                   {"reason", ci.lastReason},
                   {"remote", ci.remoteUri},
                   {"remoteContact", ci.remoteContact},
                   {"direction", ci.role == PJSIP_ROLE_UAC ? "out" : "in"},
                   {"durationSec", (int)ci.connectDuration.sec}};
        emit(ev);
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED) emit({{"event", "call_ended"}, {"id", ci.id}, {"status", (int)ci.lastStatusCode}, {"reason", ci.lastReason}});
    }

    void onCallMediaState(OnCallMediaStateParam &) override {
        CallInfo ci = getInfo();
        bool active = false;
        for (unsigned i = 0; i < ci.media.size(); ++i) {
            if (ci.media[i].type != PJMEDIA_TYPE_AUDIO) continue;
            Media *m = getMedia(i);
            auto *am = m ? dynamic_cast<AudioMedia *>(m) : nullptr;
            if (!am) continue;
            if (ci.media[i].status == PJSUA_CALL_MEDIA_ACTIVE) {
                active = true;
                AudDevManager &mgr = Endpoint::instance().audDevManager();
                if (!muted) mgr.getCaptureDevMedia().startTransmit(*am);
                am->startTransmit(mgr.getPlaybackDevMedia());
            }
        }
        emit({{"event", "media"}, {"id", ci.id}, {"audioActive", active}});
    }

    void onDtmfDigit(OnDtmfDigitParam &prm) override { emit({{"event", "dtmf_in"}, {"id", getId()}, {"digit", prm.digit}}); }

    void onCallTransferStatus(OnCallTransferStatusParam &prm) override {
        emit({{"event", "transfer_status"}, {"id", getId()}, {"status", (int)prm.statusCode}, {"reason", prm.reason}, {"final", prm.finalNotify}});
        prm.cont = true;
    }

    void playFile(const std::string &file) {
        player.reset(new EnginePlayer());
        player->callId = getId();
        player->createPlayer(file, PJMEDIA_FILE_NO_LOOP);
        player->startTransmit(getAudioMedia(-1));
    }

    void setMuted(bool value) {
        muted = value;
        CallInfo ci = getInfo();
        AudDevManager &mgr = Endpoint::instance().audDevManager();
        for (unsigned i = 0; i < ci.media.size(); ++i) {
            if (ci.media[i].type != PJMEDIA_TYPE_AUDIO || ci.media[i].status != PJSUA_CALL_MEDIA_ACTIVE) continue;
            auto *am = dynamic_cast<AudioMedia *>(getMedia(i));
            if (!am) continue;
            if (value) mgr.getCaptureDevMedia().stopTransmit(*am);
            else mgr.getCaptureDevMedia().startTransmit(*am);
        }
    }
};

EngineCall *find_call(int id) {
    std::lock_guard<std::mutex> lock(g.calls_mutex);
    for (auto &c : g.calls) {
        if (c && c->isActive() && c->getId() == id) return c.get();
    }
    return nullptr;
}

void sweep_calls() {
    std::lock_guard<std::mutex> lock(g.calls_mutex);
    for (auto it = g.calls.begin(); it != g.calls.end();) {
        if (!(*it)->isActive()) it = g.calls.erase(it);
        else ++it;
    }
}

class EngineAccount : public Account {
public:
    void onRegState(OnRegStateParam &prm) override {
        AccountInfo ai = getInfo();
        emit({{"event", "reg_state"},
              {"code", (int)prm.code},
              {"reason", prm.reason},
              {"active", ai.regIsActive},
              {"expiresSec", (int)ai.regExpiresSec}});
    }

    void onIncomingCall(OnIncomingCallParam &iprm) override {
        sweep_calls();
        auto call = std::make_unique<EngineCall>(*this, iprm.callId);
        CallInfo ci = call->getInfo();
        if (g.dnd.load()) {
            // Não perturbe: nada de "180 Ringing". A central recebe "486 Busy Here" já na primeira resposta, então
            // quem ligou ouve ocupado e o telefone nem chega a tocar. O objeto fica em g.calls até a chamada encerrar.
            CallOpParam ocupado;
            ocupado.statusCode = PJSIP_SC_BUSY_HERE;
            call->answer(ocupado);
            emit({{"event", "incoming_blocked"}, {"id", ci.id}, {"remote", ci.remoteUri}});
            std::lock_guard<std::mutex> lock(g.calls_mutex);
            g.calls.push_back(std::move(call));
            return;
        }
        // Toca "180 Ringing" logo para a central saber que o telefone está avisando a pessoa.
        CallOpParam ringing;
        ringing.statusCode = PJSIP_SC_RINGING;
        call->answer(ringing);
        emit({{"event", "incoming"}, {"id", ci.id}, {"remote", ci.remoteUri}, {"remoteContact", ci.remoteContact}});
        std::lock_guard<std::mutex> lock(g.calls_mutex);
        g.calls.push_back(std::move(call));
    }
};

// ---- comandos ----------------------------------------------------------------------------------

std::string str(const json &j, const char *key, const std::string &fallback = "") {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}
int num(const json &j, const char *key, int fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int>() : fallback;
}
bool flag(const json &j, const char *key, bool fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

// Aceita só dígitos, + * # (e letras para nomes de usuário SIP simples); impede injetar cabeçalhos ou URIs.
bool valid_target(const std::string &s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        bool ok = (c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#' || c == '.' || c == '-' || c == '_' ||
                  (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!ok) return false;
    }
    return true;
}
bool valid_host(const std::string &s) {
    if (s.empty() || s.size() > 253) return false;
    for (char c : s) {
        bool ok = (c >= '0' && c <= '9') || c == '.' || c == '-' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!ok) return false;
    }
    return true;
}

void cmd_init(const json &m) {
    if (g.started) return emit_error("init", "O motor já foi iniciado.");
    g.ep.libCreate();
    EpConfig cfg;
    cfg.uaConfig.userAgent = str(m, "userAgent", std::string("WorkscapePhone/") + kVersion);
    cfg.uaConfig.maxCalls = 8;
    cfg.logConfig.level = num(m, "logLevel", 3);
    cfg.logConfig.consoleLevel = 0;  // nada no console (stdout): o log passa pelo writer, em stderr
    cfg.logConfig.writer = &g_log_writer;
    cfg.logConfig.msgLogging = 0;  // não imprimir mensagens SIP completas (têm cabeçalhos de autenticação)
    cfg.medConfig.ecTailLen = num(m, "echoTailMs", 200);
    cfg.medConfig.noVad = !flag(m, "vad", false);
    if (m.contains("stun") && m["stun"].is_array())
        for (auto &s : m["stun"]) if (s.is_string()) cfg.uaConfig.stunServer.push_back(s.get<std::string>());
    g.nullAudio = flag(m, "nullAudio", false);
    g.ep.libInit(cfg);

    std::string transport = str(m, "transport", "udp");
    TransportConfig tcfg;
    tcfg.port = (unsigned)num(m, "sourcePort", 0);
    if (transport == "tcp") g.ep.transportCreate(PJSIP_TRANSPORT_TCP, tcfg);
    else g.ep.transportCreate(PJSIP_TRANSPORT_UDP, tcfg);

    g.ep.libStart();
    if (g.nullAudio) g.ep.audDevManager().setNullDev();

    // Prioridade dos codecs; lista do mais para o menos preferido. Os que não existirem são ignorados.
    std::vector<std::string> codecs = {"PCMA/8000/1", "PCMU/8000/1", "G722/16000/1", "GSM/8000/1"};
    if (m.contains("codecs") && m["codecs"].is_array()) {
        codecs.clear();
        for (auto &c : m["codecs"]) if (c.is_string()) codecs.push_back(c.get<std::string>());
    }
    for (auto &c : g.ep.codecEnum2()) g.ep.codecSetPriority(c.codecId, 0);
    int prio = 255;
    for (auto &c : codecs) {
        try { g.ep.codecSetPriority(c, (pj_uint8_t)prio--); } catch (Error &) { /* codec ausente neste build */ }
    }
    g.started = true;
    json ev = {{"event", "ready"}, {"version", kVersion}, {"pjsip", pj_get_version()}, {"transport", transport}, {"nullAudio", g.nullAudio}};
    emit(ev);
}

void cmd_register(const json &m) {
    if (!g.started) return emit_error("register", "Envie 'init' antes.");
    std::string user = str(m, "user"), pass = str(m, "password"), domain = str(m, "domain");
    std::string server = str(m, "server", domain), transport = str(m, "transport", "udp");
    if (!valid_target(user) || !valid_host(domain) || !valid_host(server) || pass.empty())
        return emit_error("register", "Dados da conta inválidos.");
    g.secret = pass;
    g.domain = domain;
    int port = num(m, "port", 5060);
    std::string display = str(m, "displayName");
    if (display.find('"') != std::string::npos || display.find('\r') != std::string::npos || display.find('\n') != std::string::npos)
        return emit_error("register", "Nome de exibição inválido.");

    AccountConfig acfg;
    acfg.idUri = (display.empty() ? "" : "\"" + display + "\" ") + std::string("<sip:") + user + "@" + domain + ">";
    std::string suffix = transport == "tcp" ? ";transport=tcp" : "";
    acfg.regConfig.registrarUri = "sip:" + server + ":" + std::to_string(port) + suffix;
    // A central VIP impõe expiração mínima de 60 s; renovar antes disso é responsabilidade do PJSIP.
    acfg.regConfig.timeoutSec = (unsigned)num(m, "registerRefresh", 60);
    acfg.regConfig.retryIntervalSec = (unsigned)num(m, "retryIntervalSec", 20);
    acfg.sipConfig.authCreds.push_back(AuthCredInfo("digest", "*", user, 0, pass));
    acfg.natConfig.udpKaIntervalSec = (unsigned)num(m, "keepAlive", 15);
    acfg.natConfig.contactRewriteUse = 1;
    acfg.natConfig.viaRewriteUse = flag(m, "viaRewrite", true) ? 1 : 0;
    acfg.natConfig.sdpNatRewriteUse = flag(m, "sdpRewrite", true) ? 1 : 0;
    acfg.mediaConfig.srtpUse = PJMEDIA_SRTP_DISABLED;

    g.account.reset(new EngineAccount());
    g.account->create(acfg);
}

void cmd_unregister() {
    if (g.account) g.account->setRegistration(false);
}

void cmd_call(const json &m) {
    if (!g.account) return emit_error("call", "Registre a conta antes.");
    std::string number = str(m, "number");
    if (!valid_target(number)) return emit_error("call", "Número inválido.");
    // O domínio vem da conta, nunca do comando: o front não consegue redirecionar chamadas para outro servidor.
    std::string uri = "sip:" + number + "@" + g.domain;
    auto call = std::make_unique<EngineCall>(*g.account);
    CallOpParam prm(true);
    call->makeCall(uri, prm);
    int id = call->getId();
    {
        std::lock_guard<std::mutex> lock(g.calls_mutex);
        g.calls.push_back(std::move(call));
    }
    emit({{"event", "call_started"}, {"id", id}, {"number", number}});
}

void with_call(const json &m, const char *cmd, const std::function<void(EngineCall &)> &fn) {
    EngineCall *c = find_call(num(m, "id", -1));
    if (!c) return emit_error(cmd, "Chamada não encontrada.");
    fn(*c);
}

void cmd_devices() {
    if (!g.started) return emit_error("devices", "Envie 'init' antes de listar os dispositivos de áudio.");
    json out = {{"event", "devices"}, {"capture", json::array()}, {"playback", json::array()}};
    AudioDevInfoVector2 list = g.ep.audDevManager().enumDev2();
    for (size_t i = 0; i < list.size(); ++i) {
        json d = {{"id", (int)i}, {"name", list[i].name}, {"driver", list[i].driver}};
        if (list[i].inputCount > 0) out["capture"].push_back(d);
        if (list[i].outputCount > 0) out["playback"].push_back(d);
    }
    emit(out);
}

void dispatch(const json &m) {
    std::string cmd = str(m, "cmd");
    try {
        if (cmd == "init") cmd_init(m);
        else if (cmd == "register") cmd_register(m);
        else if (cmd == "unregister") cmd_unregister();
        else if (cmd == "call") cmd_call(m);
        else if (cmd == "answer") with_call(m, "answer", [](EngineCall &c) { CallOpParam p; p.statusCode = PJSIP_SC_OK; c.answer(p); });
        else if (cmd == "reject") with_call(m, "reject", [](EngineCall &c) { CallOpParam p; p.statusCode = PJSIP_SC_DECLINE; c.hangup(p); });
        else if (cmd == "hangup") with_call(m, "hangup", [](EngineCall &c) { CallOpParam p; c.hangup(p); });
        else if (cmd == "mute") with_call(m, "mute", [&](EngineCall &c) { c.setMuted(flag(m, "on", true)); emit({{"event", "mute"}, {"id", c.getId()}, {"on", c.muted}}); });
        else if (cmd == "hold") with_call(m, "hold", [](EngineCall &c) { CallOpParam p; c.setHold(p); });
        else if (cmd == "unhold") with_call(m, "unhold", [](EngineCall &c) { CallOpParam p(true); p.opt.flag = PJSUA_CALL_UNHOLD; c.reinvite(p); });
        else if (cmd == "dtmf") with_call(m, "dtmf", [&](EngineCall &c) {
            std::string d = str(m, "digits");
            for (char ch : d) if (!((ch >= '0' && ch <= '9') || ch == '*' || ch == '#' || (ch >= 'A' && ch <= 'D'))) return emit_error("dtmf", "Dígito inválido.");
            c.dialDtmf(d);
        });
        else if (cmd == "transfer") with_call(m, "transfer", [&](EngineCall &c) {
            std::string to = str(m, "to");
            if (!valid_target(to) || !g.account) return emit_error("transfer", "Destino inválido.");
            CallOpParam p;
            c.xfer("sip:" + to + "@" + g.domain, p);
        });
        else if (cmd == "play") with_call(m, "play", [&](EngineCall &c) {
            std::string f = str(m, "file");
            bool wav = f.size() > 4 && f.compare(f.size() - 4, 4, ".wav") == 0;
            if (!wav || f.find("..") != std::string::npos) return emit_error("play", "Só arquivos .wav, sem '..' no caminho.");
            c.playFile(f);
            emit({{"event", "play_started"}, {"id", c.getId()}});
        });
        else if (cmd == "dnd") { g.dnd.store(flag(m, "on", false)); emit({{"event", "dnd"}, {"on", g.dnd.load()}}); }
        else if (cmd == "devices") cmd_devices();
        else if (cmd == "set_devices") {
            if (!g.started) return emit_error("set_devices", "Envie 'init' antes.");
            AudDevManager &mgr = g.ep.audDevManager();
            if (m.contains("capture") && m["capture"].is_number()) mgr.setCaptureDev(m["capture"].get<int>());
            if (m.contains("playback") && m["playback"].is_number()) mgr.setPlaybackDev(m["playback"].get<int>());
            emit({{"event", "devices_set"}});
        }
        else if (cmd == "ping") emit({{"event", "pong"}, {"t", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}});
        else if (cmd == "quit") { /* tratado no laço principal */ }
        else emit_error(cmd, "Comando desconhecido.");
    } catch (Error &e) {
        emit_error(cmd, scrub(e.reason, g.secret), e.status);
    } catch (std::exception &e) {
        emit_error(cmd, scrub(e.what(), g.secret));
    }
}

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    emit({{"event", "hello"}, {"version", kVersion}});
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // Windows entrega \r\n
        if (line.empty()) continue;
        json m = json::parse(line, nullptr, false);
        if (m.is_discarded() || !m.is_object()) { emit_error("", "JSON inválido."); continue; }
        if (str(m, "cmd") == "quit") break;
        dispatch(m);
    }
    // Encerramento limpo: cancela o registro e destrói a biblioteca.
    try {
        if (g.account) { g.account->setRegistration(false); pj_thread_sleep(300); }
        g.calls.clear();
        g.account.reset();
        if (g.started) g.ep.libDestroy();
    } catch (...) {}
    emit({{"event", "bye"}});
    return 0;
}
