# Workscape Phone Engine

Motor de softphone SIP em C++ sobre o **PJSUA2** (PJSIP 2.15.1). Roda como um processo separado e é controlado por um protocolo JSON de uma mensagem por linha, via stdin e stdout. Foi feito para ser embutido em um app Electron, mas funciona com qualquer programa que consiga abrir um processo filho.

Faz parte do **Workscape**, plataforma desktop interna de um provedor de internet. O motor substitui o uso do MicroSIP como telefone dos atendentes.

## Por que um processo separado

- **Isolamento.** Uma falha na pilha SIP ou de áudio derruba só o motor, não o app que o chamou.
- **Segurança.** O renderer do Electron nunca toca em credenciais SIP. A senha chega só no comando `register` e fica apenas na memória do processo.
- **Simplicidade.** A interface é texto: dá para testar o motor inteiro com `echo` e um terminal.

## Recursos

- Registro em central SIP (UDP ou TCP), com reconexão e eventos de estado.
- Chamadas de saída e de entrada, atender, recusar e encerrar.
- Mudo, espera (hold) e retomar, transferência (REFER) com acompanhamento de status.
- DTMF de saída e de entrada.
- Não perturbar: responde `486 Busy Here` sem tocar.
- Seleção de dispositivos de captura e reprodução, e níveis de áudio (microfone e remoto) em tempo real.
- Prioridade de codecs configurável (padrão: PCMA, PCMU, G.722, GSM).
- Reprodução de arquivo `.wav` dentro da chamada (usada nos testes).

## Protocolo

Cada linha é um objeto JSON. Comandos entram por **stdin** e eventos saem por **stdout**. Logs de diagnóstico vão para **stderr** e nunca incluem senhas nem mensagens SIP completas.

Exemplo (`->` envia, `<-` recebe):

```text
<- {"event":"hello","version":"0.1.0"}
-> {"cmd":"init","transport":"udp"}
<- {"event":"ready","version":"0.1.0","pjsip":"2.15.1","transport":"udp"}
-> {"cmd":"register","user":"1001","password":"***","domain":"pbx.exemplo.com.br"}
<- {"event":"reg_state", ...}
-> {"cmd":"call","number":"1002"}
<- {"event":"call_started","id":0,"number":"1002"}
<- {"event":"call_state", ...}
-> {"cmd":"hangup","id":0}
<- {"event":"call_ended","id":0,"status":200,"reason":"Normal call clearing"}
-> {"cmd":"quit"}
<- {"event":"bye"}
```

| Comando | Função |
|---|---|
| `init` | Inicia o PJSIP (transporte, codecs, STUN, eco, VAD) |
| `register`, `unregister` | Registra ou remove o registro na central |
| `call`, `answer`, `reject`, `hangup` | Controle da chamada |
| `mute`, `hold`, `unhold` | Estado de mídia da chamada |
| `dtmf`, `transfer` | Tons e transferência |
| `play` | Toca um `.wav` na chamada |
| `dnd` | Liga ou desliga o Não perturbe |
| `devices`, `set_devices` | Lista e escolhe os dispositivos de áudio |
| `ping`, `quit` | Verificação de vida e encerramento |

| Evento | Quando |
|---|---|
| `hello`, `ready`, `bye` | Ciclo de vida do processo |
| `reg_state` | Mudança no registro |
| `incoming`, `incoming_blocked` | Chamada recebida, ou barrada pelo Não perturbe |
| `call_started`, `call_state`, `call_ended` | Ciclo da chamada |
| `media`, `mute`, `levels` | Mídia ativa, mudo, níveis de áudio |
| `dtmf_in`, `transfer_status` | DTMF recebido, andamento da transferência |
| `devices`, `devices_set`, `dnd`, `play_started`, `play_done`, `pong` | Respostas aos comandos |
| `error` | Falha em um comando, com `cmd`, `message` e `code` |

## Decisões de segurança

- Destinos de chamada e de transferência passam por uma lista de caracteres permitidos, o que impede injeção de cabeçalhos SIP ou de URIs.
- O comando `play` aceita só `.wav` e rejeita `..` no caminho.
- Mensagens de erro do PJSIP passam por um filtro que remove a senha antes de sair.
- Os logs do PJSIP vão só para stderr, deixando o stdout reservado ao protocolo.
- O PJSIP é baixado por versão fixa e o build confere o **SHA-256** do arquivo antes de compilar.

## Como compilar

### Windows x64 (GitHub Actions)

O workflow [`phone-engine-windows.yml`](.github/workflows/phone-engine-windows.yml) baixa o PJSIP 2.15.1, confere o hash, compila em Release-Static com runtime `/MT` e gera `workscape-phone-engine.exe` como artefato e como Release. Dispara em `push` na `main` quando algo em `phone-engine/` muda, ou manualmente.

### Linux (desenvolvimento)

Precisa do PJSIP já compilado em `third_party/pjproject-2.15.1`:

```bash
cd phone-engine
make            # gera bin/workscape-phone-engine
```

Para testar sem placa de som, envie `"nullAudio": true` no comando `init`.

## Estrutura

```text
phone-engine/
  src/main.cpp           motor e protocolo
  third_party/json.hpp   nlohmann/json 3.11.3
  Makefile               build de desenvolvimento no Linux
.github/workflows/       build do executável para Windows
docs/LICENCAS.md         licenças dos componentes
```

## Estado do projeto

- Registro e chamada testados no Linux contra uma central Asterisk real.
- Build de Windows verde no GitHub Actions.
- **Ainda não validado:** áudio real no Windows. Melhorias previstas: nomes de dispositivos completos via WASAPI e SIP sobre TLS.

## Licença

Este programa liga o **PJSIP**, distribuído sob **GPL v2** ou licença comercial da Teluu. O uso e o estudo seguem a GPL. Distribuir ou vender um binário exige licença comercial do PJSIP ou trocar de motor (por exemplo, baresip, de licença BSD). Detalhes em [docs/LICENCAS.md](docs/LICENCAS.md).
