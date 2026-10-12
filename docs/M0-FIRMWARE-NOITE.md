# M0 — Preparação noturna do firmware (2026-10-11)

Rodada executada autonomamente por Claude, sem supervisão em tempo real
(autorização prévia de Rodrigo, 2026-10-11 03:09, antes de dormir). Worktree
isolado, sem rede real, sem hardware, sem broker, sem backend, sem Docker.

## Resumo executivo

1. **Baseline**: `main` = `origin/main` = HEAD do worktree =
   `0dbc37e8cda71ccdaacd0ae5e896f742a5f08e36`, exatamente o esperado. Sem
   divergência, sem outro worktree concorrente.
2. **Resultado final**: todas as tarefas (T1–T5) concluídas. Nenhum gate
   falhou. Nenhuma mudança em código produtivo.
3. **Contagem de checks**: baseline QEMU `394/394 ALL_PASS` → após T1,
   `398/398 ALL_PASS` (4 checks novos: 2 fixtures × {ESP_OK, bytes exatos}).
   Zero falhas, zero panic, zero watchdog nas duas rodadas.
4. **Arquivos adicionados/modificados** (ver lista completa na seção
   "Commits locais"): 2 fixtures novas, 1 CMakeLists de validação, 1
   main.cpp de validação (só testes), este documento.
5. **Tarefas**: T1 concluída e commitada. T2, T3, T4, T5 concluídas
   (somente análise/documentação, sem código produtivo). Nenhuma tarefa
   ficou parcial ou bloqueada.
6. **Impedimentos concretos**: nenhum que tenha impedido a entrega desta
   rodada. O único "pendente" real é estrutural e esperado: não existe
   fixture P1 oficial 31×12 no repositório ainda, então a bijeção completa
   do ledmap da bancada não pode ser testada agora — exatamente como a
   tarefa já previa e proibia de inventar.

---

## Fixtures

Todos os bytes abaixo vieram dos serializers reais (`pixels_firmware::serializeEvent/serializeState/serializeCapabilityReport`, em `components/pixels_firmware_contract/firmware_contract.cpp`), capturados por um gerador temporário removido antes do commit (ver histórico do commit 1, SHA abaixo). As 5 fixtures marcadas "já existia" foram comparadas byte a byte contra a saída atual do serializer pela suíte de validação já existente, que passou (394/394 no baseline, 398/398 após T1) — ou seja, estão confirmadas, não apenas assumidas.

| Mensagem | Arquivo | Bytes | SHA-256 | Newline final | Serializer | Commit que congelou |
|---|---|---|---|---|---|---|
| command_received | `validation/fixtures/event-command-received.provisional.json` | 183 | `a270d90e3061a4ef81ec844d1f6ca4fb8501aa9f9fd4f515e28038705455d147` | não | `serializeEvent()` | `dbe01081ebecbf682443c743c9b6dd58c2d988e1` |
| command_accepted | `validation/fixtures/event-command-accepted.provisional.json` | 183 | `9d997f83443213304e3d854eed5e390c0733744fd39eb45de9e52169d0ff38f9` | não | `serializeEvent()` | `dbe01081ebecbf682443c743c9b6dd58c2d988e1` |
| command_rejected | `validation/fixtures/event-checksum-mismatch-rejected.provisional.json` | 265 | `be5f6e0f094be7ad708dad31dc859e6db5fc95bcb494f674d1e3676a6aee39d4` | não | `serializeEvent()` | já existia (pré-M0); reconfirmada byte a byte nesta rodada |
| command_applied | `validation/fixtures/event-command-applied.provisional.json` | 182 | `1e5a888f6dbcc86a77bfddde2c0f63a838bc4c78148d3278cb52b458f007942a` | não | `serializeEvent()` | já existia; reconfirmada |
| command_failed | `validation/fixtures/event-command-failed.provisional.json` | 244 | `ca583c8ff4d1cd1073b2a1e34e18315dbfaa33964820caf03d684b251b58fd48` | não | `serializeEvent()` | já existia; reconfirmada |
| capability_reported | `validation/fixtures/event-capability-reported.provisional.json` | 739 | `d31dc7622ba59281f4a96f8616fc3fb4f84ff43baee4d0809dd1af4e439cb46d` | não | `serializeCapabilityReport()` | já existia; reconfirmada |
| state | `validation/fixtures/state-config-active.provisional.json` | 353 | `fd5aa1045a6c32911585633da0af7779a5f7c4894260978d07d235c3020b8e4c` | não | `serializeState()` | já existia; reconfirmada |

**Nota de método sobre SHA-256**: o cálculo acima foi feito no host
(`sha256sum` sobre o arquivo no checkout), não dentro do firmware/QEMU. A
suíte de validação permanente compara **bytes exatos** (`memcmp`) entre o
serializer real e o arquivo incorporado via `EMBED_TXTFILES` — o mesmo
padrão já usado pelas 5 fixtures que já existiam antes desta rodada. Não
introduzi um segundo mecanismo (um hash SHA-256 calculado dentro do
firmware) só para estas duas novas fixtures, porque isso quebraria a
convenção já estabelecida: SHA-256 dentro do firmware é reservado para o
contrato real de payload (`apply_config`/`activate_preset`, verificado via
PSA crypto), não para igualdade de fixtures de teste. O `NUL` final que o
`EMBED_TXTFILES` acrescenta é descontado por `fixtureSize()`
(`end - start - 1`), seguindo o padrão já existente — nenhuma fixture nova
precisou de tratamento especial aqui.

## Campos de correlação

Levantado diretamente das estruturas em `firmware_contract.hpp`:

| Campo | `StateReport` (state) | `CapabilityReport` (capability_reported) | `Event` (5 ACKs: received/accepted/rejected/applied/failed) |
|---|---|---|---|
| `device_id` | sim | sim | sim |
| `command_id` | não (tem `last_applied_command_id`, não é o mesmo campo) | não | sim |
| `hardware_binding_id` | sim | sim | **não** |
| `generation` | sim | sim | **não** |
| `boot_counter` | sim | não | não |
| `state_sequence` | sim | não | não |

**Hipótese do brief — confirmada exatamente**:

| Mensagem | Binding | Geração | Contadores |
|---|---|---|---|
| state | Sim | Sim | `boot_counter` e `state_sequence` |
| capability_reported | Sim | Sim | Nenhum |
| Cinco ACKs | Não | Não | Nenhum |

**Observação** (não é um gap a corrigir, só um registro, conforme pedido): os
cinco eventos ACK se correlacionam com o resto do sistema **somente** por
`device_id` + `command_id`. Não adicionei `hardware_binding_id`, `generation`
nem contadores a eles — isso mudaria o contrato de mensagens, que está fora
desta rodada.

---

## Limites

### `capabilities.max_payload_bytes` = 8192

Confirmado em `components/pixels_payload_adapter/include/pixels_payload_adapter.hpp:33`
(`constexpr size_t kMaxPayloadBytes = 8192;`) e publicado em `main.cpp`'s
`capabilityManifest()` via `manifest.max_payload_bytes = pixels_payload::kMaxPayloadBytes;`
(entregue na rodada F1 anterior).

**O que exatamente é contado**: os bytes exatos do **payload já resolvido**
de `apply_config`/`activate_preset` — ou seja, o JSON de configuração ou
preset completo, depois que `PayloadSource::resolve()` já trouxe os bytes
reais (do fixture embutido ou do download HTTPS). **Não é** o envelope do
comando MQTT (esse tem seu próprio limite, ver abaixo) nem o tamanho do
comando JSON (`apply_config`/`activate_preset` em si, que só carrega
metadados como URL/checksum, não o payload).

**Onde o limite é aplicado** (confirmado lendo o código, não suposto):

1. **Buffer do executor** — `pixels_command_executor.hpp:173`:
   `char payload_buffer_[pixels_payload::kMaxPayloadBytes]{};` é o destino
   fixo que `payload_source_.resolve(command_, payload_buffer_,
   sizeof(payload_buffer_), ...)` recebe. `sizeof(payload_buffer_)` já é
   8192 por construção — não há como um resolver devolver mais do que isso,
   o buffer simplesmente não existe maior.
2. **Download HTTPS** — `pixels_control_runtime.cpp:247`:
   `request.capacity = std::min(capacity, config_.max_payload_bytes);` — o
   teto efetivo do download é o **menor** entre o buffer do chamador (8192,
   do executor) e o teto configurado (`HttpsPayloadSource::Config::max_payload_bytes`,
   que por padrão também é `pixels_payload::kMaxPayloadBytes` mas pode ser
   configurado **mais baixo**, nunca mais alto — `validHttpsConfig()` rejeita
   um valor configurado acima de 8192). O corpo HTTP é acumulado
   incrementalmente e rejeitado (`ESP_ERR_INVALID_SIZE`) se exceder esse teto
   antes de completar.
3. **Parsing** — `pixels_payload_adapter.cpp` (`parseConfigPayload`,
   `parsePresetPayload`, `materializeLedmap`): cada uma rejeita
   explicitamente `size > kMaxPayloadBytes` com `SchemaError::OutOfRange`,
   mesmo que o executor já garanta isso pelo tamanho do buffer — é uma
   segunda trava independente, não decorativa (a função pode em teoria ser
   chamada por outro caller com um buffer maior).
4. **Staging durável** — `pixels_durable_storage.hpp:154` define sua
   **própria** constante `kMaxPayloadBytes = 8192` (mesmo valor, constante
   separada, não a mesma do `pixels_payload_adapter`) usada em
   `kWireBytes` e verificada em `pixels_durable_storage.cpp:604`
   (`total_size > kMaxPayloadBytes` → erro). Ou seja, há **três** travas
   independentes pinadas no mesmo valor (8192), não uma única constante
   compartilhada ponta a ponta — isso é uma coincidência de valor
   deliberada, não uma garantia estrutural de que mudar uma muda as outras.

**Payload vazio ou excedente**: vazio (`payload_size == 0`) é rejeitado no
executor (`command_rejected`/`resolve_failed`) e também no download HTTPS
(`result.size == 0` → `ESP_ERR_INVALID_SIZE`) e no parser
(`SchemaError::Malformed`). Excedente nunca chega a existir como bytes
completos no buffer do executor — é cortado/rejeitado antes de completar,
em qualquer uma das três camadas acima.

**Hipótese do brief — confirmada**: é o limite dos bytes exatos do payload
resolvido (config ou preset), não do envelope MQTT. Não alterei o valor.

### `NetworkSession::kMaxMessageBytes` = 1024

Confirmado em `components/pixels_network/include/network_session.hpp:22`.
Este é o limite da **mensagem MQTT recebida já remontada** (o comando
`apply_config`/`activate_preset`/etc. em si, não o payload resolvido acima —
são dois limites de camadas diferentes, nunca comparados um contra o outro).

**Remontagem de fragmentos**: `MessageBuffer<1024>::append()`
(`message_buffer.hpp`) recebe cada fragmento MQTT com seu `offset` e `total`
anunciados pelo SDK. **Antes de copiar qualquer byte**, verifica
`total > Capacity` (linha 26) — se o `total` anunciado já excede 1024, a
mensagem é **imediatamente rejeitada** (`Result::Rejected`), mesmo que seja
só o primeiro fragmento.

**O que acontece quando `total_data_len` excede o limite**: o buffer
chama `reset()` e devolve `Rejected`. No `NetworkSession::event()`
(`MQTT_EVENT_DATA`), isso incrementa `++rejected_;`. A mensagem **nunca**
chega à fila (`messages_`)/ao `receive()`/ao executor — não há
`extractCommandId()`, não há parse, não há nenhuma tentativa de identificar
o comando.

**Contador incrementado**: somente `rejected_` (contador diagnóstico de
`NetworkSession::Status`). **Não** é `overflow_` — esse é só para quando
uma mensagem *completa* não cabe na fila de 2 posições; são cenários
diferentes e não devem ser confundidos.

**A mensagem chega ao executor?** Não.

**É possível emitir um ACK terminal nesse caso?** **Não.** Confirmado
exatamente como o brief pediu que eu verificasse sem presumir: como a
mensagem é descartada antes mesmo de `current_data_offset == 0`'s checagem
de tópico ou de qualquer tentativa de `extractCommandId()`, **nenhum
`command_id` jamais existe para esta mensagem**. Portanto **nenhum ACK
correlacionável pode ser produzido** — nem `command_rejected`, nem nada. O
único sinal observável é o contador `rejected_` subindo, sem nenhuma
associação a um comando específico.

**Hipótese do brief — confirmada, sem presumir `command_rejected`.** Não
alterei o limite.

### Relógio e NTP

1. **`UtcTimestampSource::now()`** (`pixels_control_runtime.cpp:322-332`):
   chama `std::time(nullptr)` e formata via `gmtime_r`/`strftime`. O
   fallback hardcoded `"1970-01-01T00:00:00Z"` só é usado se `gmtime_r`
   **falhar** ou `strftime` devolver 0 — uma falha de formatação, não
   especificamente "relógio não sincronizado". Antes do NTP, `std::time()`
   retorna um valor próximo de zero (ou o que a RTC tiver), e `gmtime_r`
   formata isso **sem falhar**, produzindo um timestamp sintaticamente
   válido mas semanticamente errado (uma data de 1970 real, não o literal
   de fallback). Este `now()` em si não tem nenhum gate de "relógio
   válido" — ele sempre devolve *algo*.
2. **Gate produtivo em `NetworkSession::poll()`** (`network_session.cpp:104`):
   `if (std::time(nullptr) < 1704067200) { return ESP_OK; }` —
   `1704067200` é exatamente `2024-01-01T00:00:00Z`. Esta checagem roda
   **antes** de `esp_mqtt_client_start()` ser chamado e **antes** de
   `mqtt_started_` virar `true`. O Wi-Fi já pode estar conectado nesse
   ponto (a checagem de Wi-Fi vem antes); é especificamente o **MQTT** que
   fica bloqueado.
3. **MQTT pode iniciar antes do relógio atingir o limiar?** **Não.**
   `esp_mqtt_client_start()` simplesmente não é chamado enquanto
   `std::time(nullptr) < 1704067200`. Sem isso, não há `MQTT_EVENT_CONNECTED`,
   não há `subscribed_`, e `publish()`/`publishStatus()` dependem de
   `subscribed_ == true` — ou seja, nenhuma publicação MQTT de qualquer tipo
   é fisicamente possível antes desse limiar, não é uma questão de o
   timestamp estar "errado", é que o transporte inteiro não existe ainda.
4. **state/capability_reported/ACKs podem ser publicados antes da
   sincronização?** **Não**, pelo motivo acima — é uma consequência direta
   do gate em `poll()`, não uma checagem separada em cada publicação.

**Hipótese do brief — confirmada exatamente**: o fallback de 1970 existe no
`UtcTimestampSource`, mas a sessão produtiva nunca inicia MQTT com o relógio
antes de 2024, então o fluxo normal nunca chega a publicar esse timestamp
via MQTT. Não alterei este comportamento.

**O que isso implica para T5 (bancada)**: o SNTP é bootstrapado pela
aplicação (`main.cpp`, via `esp_netif_sntp_*`, conforme o README já
documenta) depois de obter IP — isso é responsabilidade do produto, não de
`NetworkSession`. A renderização da configuração durável acontece **antes**
de a rede ser inicializada no boot (ver `main.cpp`'s ordem de boot: staging
durável → topologia segura/ativa → só então rede), então ela nunca depende
do NTP terminar.

### Comandos ainda não suportados (`reboot`/`ota`)

Confirmado com o teste **já existente** em
`validation/main/main.cpp` (herdado da rodada F1, já mesclado em
`0dbc37e`): `reboot_executor.execute(kReboot, ...)` e o mesmo executor
reutilizado para `kOta` (para não duplicar o buffer de 8 KiB de um segundo
`Executor`) resultam ambos em
`event_sink6.anyContains("command_rejected") && event_sink6.anyContains("unsupported_command")`
— confirmado passando no baseline G0 (394/394) e em G1 (398/398) desta
rodada, sem nenhuma mudança nesse teste. `reason_code` vem de
`parseErrorCode(ParseError::UnsupportedCommand)`, o mesmo código usado para
um tipo de envelope desconhecido — não um código próprio de "não
implementado". Não implementei `reboot` nem `OTA`.

---

## Topologia

Separando explicitamente, como pedido:

### Perfil físico compilado (firmware, `main.cpp:197-206`)

```cpp
board_outputs[0] = {slot_index=0, gpio=16, max_pixels=2048 (kMaxPixelsPerOutput),
                     safe_pixel_count=372, safe_order=GRB}
board_outputs[1] = {slot_index=1, gpio=17, max_pixels=2048,
                     safe_pixel_count=0,   safe_order=GRB}
```

- GPIO16 confirmado, slot lógico 0.
- Capacidade segura de 372 pixels confirmada (`safe_pixel_count=372`) — é
  exatamente quantos pixels são limpos/renderizados em preto quando não há
  configuração durável nenhuma, batendo com os 372 pixels físicos da
  bancada (31×12).
- **Nuance a registrar com precisão** (não é uma falha, é uma distinção que
  vale deixar explícita): GPIO17/slot 1 tem `safe_pixel_count=0` — ou seja,
  no estado seguro de boot (sem config), ele contribui zero pixels, que é o
  que a bancada observa hoje ("nenhuma outra GPIO está ativa"). Mas o
  **perfil compilado** ainda declara slot 1 como uma saída lógica válida,
  com `max_pixels=2048` — se uma config remota algum dia apontar
  `slot_index:1`, o firmware tentaria de fato acionar GPIO17. Isso não é um
  bug: é esperado para uma placa com 2 saídas físicas suportadas pelo
  firmware onde só uma está fisicamente fiada nesta bancada específica. Só
  registro para não confundir "nenhuma GPIO ativa agora" com "GPIO17 está
  removida do perfil" — não está.

### Dirigido por payload (não por firmware)

Busquei por qualquer algoritmo de serpentina/zigzag no firmware e nas
dependências (`esp_pixels_composer`, `esp_pixels_fx`) — **não existe
nenhum**. A única menção a "serpentine" no repositório é uma frase
descritiva no README do `esp-pixels-composer` (submodule), explicando o que
um ledmap *pode* expressar, não um gerador embutido.

`pixels_payload::LedmapCandidate` (`pixels_payload_adapter.hpp:63-68`) só
guarda `width`/`height`/`entry_count`/`configured` como resumo validado; a
tabela real (`materializeLedmap()`) copia "row-major logical indices and -1
holes" — ou seja, **quem constrói o payload decide** qual índice físico vai
em qual posição `[x,y]` do canvas, em que ordem, com que origem de canto, e
se algum ponto é buraco (`-1`). O firmware só valida (dimensões, duplicidade,
limites) e aplica a tabela — ele não tem opinião sobre origem de canto,
direção de coluna, ou se o padrão é serpentina, linear, ou qualquer outra
coisa.

**Conclusão**: a descrição da bancada (pixel 0 no canto inferior esquerdo,
colunas alternando sentido, pixel 371 no canto superior direito) é
inteiramente um fato do **payload/ledmap** que ainda será construído para
esta bancada — não uma propriedade do firmware. Não criei nenhum algoritmo
gerador, fixture contratual, campo novo ou convenção de coordenadas nova,
exatamente como instruído.

### Informação declarada por Rodrigo (não verificada em bytes nesta rodada)

GPIO16, 372 pixels, canvas 31×12, serpentina por colunas, pixel 0
inferior-esquerdo, pixel 371 superior-direito — tomada como descrição da
bancada física real, não re-verificada aqui (não há fixture para
re-verificar contra).

### O que aguarda P1

Não existe no repositório nenhuma fixture 31×12 (procurei em
`validation/fixtures/` e não há nenhuma com essas dimensões — a única
fixture com ledmap é a 4×4 canônica da PR #72). O vetor de pixels esperado
abaixo é só documentação para uso **futuro**, quando essa fixture for
congelada pelo pacote P1:

| Pixel | Posição esperada |
|---|---|
| 0 | canto inferior esquerdo |
| 11 | canto superior esquerdo |
| 12 | topo da coluna 1 |
| 23 | base da coluna 1 |
| 359 | base da coluna 29 |
| 360 | base da coluna 30 |
| 371 | canto superior direito |

Os testes completos de bijeção/aplicação aguardam essa fixture, conforme
instruído.

---

## Credencial MQTT

### PROPOSTA DO FIRMWARE — NÃO É CONTRATO CANÔNICO

Esta é uma proposta de análise independente do protocolo #47-B, só para
orientar onde a orquestração deveria viver no firmware. Não escolhe chaves
NVS, formatos wire, endpoints ou códigos de erro definitivos.

**Restrição arquitetural que motiva todo o desenho**: `NetworkSession`
mantém apenas ponteiros emprestados para as strings de `Config` (`ssid`,
`broker_uri`, `client_id`, `username`, `password`, `command_topic`, etc. —
ver `network_session.hpp:25-34`, "Configuration strings must remain valid
until end()"). Trocar identidade/credencial não é uma chamada simples:
exige `end()` (destrói o cliente MQTT atual), reconstrução de armazenamento
estável para as novas strings (a instância antiga de `Config` não pode
simplesmente ser sobrescrita enquanto `NetworkSession` ainda a referencia),
novo `init()`, novo `start()`. Essa sequência inteira é cara e observável
(reconecta do zero), então ela só deve acontecer numa transição de estado
deliberada, nunca incidentalmente.

**Estados propostos:**

1. **`CurrentConfirmed`** — a credencial que produziu o último
   `MQTT_EVENT_CONNECTED` real (que no ESP-MQTT só dispara depois de um
   CONNACK aceito pelo broker — confirmado lendo `mqtt_client.c`: o estado
   interno só vira `MQTT_STATE_CONNECTED`, e só então o evento é
   disparado, após o CONNACK). É o que `NetworkSession` usa hoje em
   produção (credencial estática).
2. **`PendingUnused`** — uma nova credencial foi emitida pela plataforma e
   persistida duravelmente (armazenamento separado de `CurrentConfirmed`),
   mas nenhuma tentativa de conexão a usou ainda. `CurrentConfirmed`
   continua intocado e em uso.
3. **`AttemptInFlight`** — o orquestrador chamou `end()` na sessão atual,
   construiu armazenamento estável para as strings da credencial pendente,
   chamou `init()`+`start()` com ela. Aguardando `MQTT_EVENT_CONNECTED`
   (= CONNACK) ou uma falha/timeout definitivo.
4. **`AmbiguousActivation`** — a tentativa foi interrompida (timeout,
   `MQTT_EVENT_DISCONNECTED`/`ERROR`, ou reboot) **antes** de
   `MQTT_EVENT_CONNECTED` ser observado. Estado crítico: como a plataforma
   pode aceitar o segredo e ativá-lo/revogar o anterior no lado do backend
   **antes** de o dispositivo confirmar CONNACK (fato já declarado no
   brief), neste estado **nenhuma das duas credenciais é segura de
   assumir** — nem a pendente (pode não ter sido de fato aceita) nem a
   atual (pode já ter sido revogada atomicamente pela plataforma ao aceitar
   a pendente).
5. **`PendingExpired`** — a credencial pendente nunca foi usada com sucesso
   dentro do seu `expires_at` (semântica exata a definir pela #47-B).
6. **`BootstrapRecovery`** — caminho terminal quando nem `CurrentConfirmed`
   nem a pendente podem ser confiados (backoff de `AmbiguousActivation`
   esgotado, ou `PendingExpired`): solicita uma credencial inteiramente
   nova pelo fluxo de bootstrap (ainda não construído).

**Transições:**

- `CurrentConfirmed` --(nova credencial emitida e persistida)--> `PendingUnused`
- `PendingUnused` --(orquestrador decide tentar a rotação)--> `AttemptInFlight`
- `AttemptInFlight` --(CONNACK observado para a credencial pendente)--> `CurrentConfirmed` (a pendente assume; só agora o registro da antiga pode ser descartado)
- `AttemptInFlight` --(timeout, disconnect/error antes do CONNACK, ou reboot no meio)--> `AmbiguousActivation`
- `AmbiguousActivation` --(retry com backoff limitado, **sempre com a mesma credencial pendente, nunca com a antiga**)--> `AttemptInFlight`
- `AmbiguousActivation` --(backoff esgotado OU `expires_at` ultrapassado)--> `PendingExpired`
- `PendingExpired` --(solicita nova credencial)--> `BootstrapRecovery`
- `BootstrapRecovery` --(nova credencial recebida e persistida)--> `PendingUnused`

**Comportamento em reboot** (ortogonal aos estados acima, aplica-se no boot
independente de qual estado foi persistido antes da queda de energia):

- Reboot com `PendingUnused` persistido, nunca tentado → no boot, retoma em
  `PendingUnused` (seguro: `CurrentConfirmed` nunca foi tocado).
- Reboot durante `AttemptInFlight` → no boot, **deve** ser tratado como
  `AmbiguousActivation`, nunca como "seguro, repetir direto" nem como
  "assumir falha, reverter para a antiga" — o próprio reboot destrói
  qualquer chance de saber se o CONNACK chegou na janela perdida.
- Reboot com `AmbiguousActivation` já persistido → permanece
  `AmbiguousActivation` (nenhuma informação nova foi obtida).

**Proibição explícita**: a partir de `AmbiguousActivation`, o orquestrador
nunca deve tentar reconectar com a credencial antiga (`CurrentConfirmed`)
como "fallback" — o único caminho para frente é repetir com a pendente
(dentro do backoff) ou escalar para `PendingExpired`→`BootstrapRecovery`.

**Preservação de renderização**: nenhuma dessas transições pode bloquear ou
depender do caminho de renderização/configuração durável, que já hoje
acontece **antes** de qualquer inicialização de rede no boot (confirmado na
seção NTP acima). Um dispositivo em `AmbiguousActivation`/`BootstrapRecovery`
deve continuar renderizando sua última configuração durável exatamente como
um dispositivo totalmente sem rede já faz hoje — rotação de credencial é um
problema de identidade do `NetworkSession`, invisível para
ativação/renderização.

**Camada**: toda essa orquestração deve viver na **aplicação/produto**
(acima de `NetworkSession`, ao lado de onde `main.cpp` hoje decide
laboratório-vs-provisionado e onde já orquestra o boot
durável-antes-da-rede) — nunca dentro do framework (`esp32libfun`) nem das
bibliotecas de pixels. `NetworkSession` continua sabendo só de "uma
`Config` ativa"; não precisa saber que existe rotação.

### Perguntas fechadas para a #47-B

1. Qual é a resposta exata do endpoint de bootstrap (campos, tipos)?
2. Qual é a resposta exata do endpoint de rotação?
3. Existe um identificador/versão explícito da credencial (para o
   dispositivo citar qual delas usou numa tentativa)?
4. `expires_at` é relativo à emissão, à primeira tentativa, ou a outra
   coisa? O que exatamente ele expressa?
5. Qual relógio avalia a expiração — o do backend (autoritativo) ou o do
   dispositivo (que pode estar pré-NTP)?
6. Como o dispositivo distingue "minha pendente expirou" de "minha
   pendente já foi ativada, só não recebi confirmação"? Existe uma consulta
   de status, ou só reemissão?
7. Quais erros são recuperáveis (retry com a mesma pendente) vs.
   definitivos (precisa de bootstrap novo)?
8. (mesma pergunta, já coberta acima — definitivos foram incluídos em 7)
9. Quando exatamente o dispositivo deve pedir uma credencial nova após uma
   ativação ambígua — imediatamente, ou só depois do backoff esgotar?
10. Existe um limite de tentativas por credencial pendente definido pela
    plataforma, ou isso é inteiramente política do firmware?
11. Backoff mínimo/máximo esperado pela plataforma (para não ser
    confundido com abuso/rate-limit)?
12. Se o dispositivo reiniciar durante a janela de rotação, o que a
    plataforma espera que ele faça — ela já sabe que pode haver essa
    ambiguidade?
13. A credencial antiga permanece utilizável por algum intervalo de graça
    depois que a plataforma aceita a nova, ou a revogação é sempre
    instantânea e atômica?
14. Qual é a evidência autoritativa de que uma ativação de fato aconteceu
    (do ponto de vista da plataforma) — só o CONNACK bem-sucedido no
    broker, ou existe também uma confirmação própria da #47-B?
15. Qual é a política de revogação — a antiga é revogada no momento em que
    a plataforma aceita a nova, ou só depois de confirmar que o dispositivo
    conectou com ela?

Não implementei testes de armazenamento falso isolado nem código produtivo
de credencial — ambos aguardam o contrato #47-B fechado, conforme instruído.

---

## Bancada — roteiro de testes futuros

Todos os casos abaixo são **NÃO EXECUTADO — requer janela coordenada de
bancada**. Nenhum QEMU ou teste sintético é apresentado como evidência de
hardware, broker ou backend real.

| # | Caso | Pré-condições | Procedimento | Evidência esperada | Critério de aceite | Tipo | Status | Dependência externa |
|---|---|---|---|---|---|---|---|---|
| 1 | Boot antes da sincronização NTP | Dispositivo sem RTC válida, rede disponível mas NTP ainda não respondeu | Energizar e observar os primeiros segundos de boot | Log mostrando render ativo antes de qualquer evento MQTT | Pixels renderizam a config durável antes do primeiro `MQTT_EVENT_CONNECTED` | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Broker real, janela de bancada |
| 2 | Renderização durável enquanto a rede aguarda NTP | Config durável válida já persistida | Cortar DNS/NTP propositalmente por alguns minutos no boot | Pixels continuam estáveis, sem flicker, sem reboot | Render nunca pausa por causa do gate de `poll()` | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Rede controlada, janela de bancada |
| 3 | Comando duplicado | Broker real, `command_id` já aplicado antes | Republicar o mesmo `apply_config` | Mesmo `command_applied` bit a bit, sem reaplicar | Replay idempotente confirmado no terminal journal | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Broker real |
| 4 | Desconexão antes do PUBACK | Publicação de state em voo | Cortar Wi-Fi/broker no meio da publicação | `publishStatus()` reporta `Unknown`, nunca falso `Acknowledged` | Nenhuma entrega falsa-positiva observada | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Broker real, controle de rede |
| 5 | Reconexão e republicação | Caso 4 completo | Restaurar rede | Mesma revisão é republicada sob novo `msg_id`, sem nova `state_sequence` | PUBACK observado para a republicação | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Broker real |
| 6 | Configuração preservada após reboot | Config ativa não-padrão | Cortar energia e religar | Mesma topologia/scene volta sem reconfiguração remota | Estado durável sobrevive ao reboot | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Janela de bancada |
| 7 | ACK terminal correspondente ao `command_id` aplicado | Comando real aplicado | Inspecionar o tópico de eventos | `command_applied` com o `command_id` exato do comando enviado | Correlação 1:1 confirmada no broker real | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Broker real |
| 8 | Keepalive ocioso via Cloudflare | Sessão MQTT estável, sem tráfego de comando por período longo | Observar por >1 keepalive interval sem enviar nada | Sessão permanece `CONNECTED`, sem desconexão silenciosa | Nenhuma reconexão inesperada no período ocioso | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Broker real atrás de Cloudflare |
| 9 | Heap livre/maior bloco antes e depois de TLS | Boot limpo | Medir heap antes do handshake TLS e depois de estabilizado | Dois números de `heap_caps_get_free_size`/`get_largest_free_block` | Sem degradação inesperada de heap sob TLS real | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Broker TLS real |
| 10 | Continuidade visual durante gravação em flash | Config ativa renderizando | Disparar uma gravação NVS/staging real | Observação visual direta dos pixels | Sem flicker/freeze perceptível durante a escrita | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Janela de bancada |
| 11 | Corte de energia durante gravação de configuração | Staging em andamento | Cortar energia no meio de `verifyAndCommit`/ativação | Reboot recupera o último estado confirmado, nunca um estado parcial | `rollback()`/recuperação de boot restauram config anterior íntegra | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | Janela de bancada |
| 12 | Corte de energia durante troca de credencial | Depende da #47-B implementada | Cortar energia durante `AttemptInFlight` (ver proposta acima) | Boot resulta em `AmbiguousActivation`, nunca falso-seguro | Nenhuma suposição indevida sobre qual credencial é válida | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | #47-B fechada e implementada |
| 13 | Reboot após persistência da credencial e antes da tentativa | Depende da #47-B implementada | Persistir `PendingUnused`, reiniciar antes de qualquer tentativa | Boot retoma em `PendingUnused`, `CurrentConfirmed` intocado | Credencial atual continua funcionando normalmente | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | #47-B fechada e implementada |
| 14 | Reboot durante ativação ambígua | Depende da #47-B implementada | Reiniciar já em `AmbiguousActivation` | Estado permanece `AmbiguousActivation` após o boot | Nenhuma promoção indevida a `CurrentConfirmed` | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | #47-B fechada e implementada |
| 15 | Recuperação por bootstrap | Depende da #47-B implementada | Forçar `PendingExpired`→`BootstrapRecovery` | Dispositivo obtém credencial nova e volta a operar | Device sai de `BootstrapRecovery` com uma `PendingUnused` nova válida | Físico | NÃO EXECUTADO — requer janela coordenada de bancada | #47-B fechada e implementada |

---

## Divergências

Só diferenças comprovadas entre código, fixtures e hipóteses:

1. **Nenhuma divergência de comportamento** foi encontrada entre o código
   real e as hipóteses do brief para `max_payload_bytes`, `kMaxMessageBytes`,
   NTP/MQTT, ou `reboot`/`ota`. Todas as quatro hipóteses foram confirmadas
   exatamente como escritas.
2. **Nuance de perfil físico** (não é bug, é uma precisão a registrar,
   detalhada na seção Topologia acima): o perfil compilado declara GPIO17
   como slot lógico 1 válido (`max_pixels=2048`), mesmo com
   `safe_pixel_count=0` e nada fisicamente conectado nesta bancada. "Nenhuma
   outra GPIO está ativa" é verdade para o estado seguro de boot, não é uma
   garantia de que uma config remota não possa endereçar GPIO17.
3. **Nenhuma fixture 31×12 existe no repositório** — confirmado por busca
   direta em `validation/fixtures/`. Isso não é uma divergência do código,
   é só a ausência esperada que o próprio brief já previa (dependente do
   pacote P1).

## Impedimentos

Nenhum impediu a conclusão desta rodada. Dependências externas concretas
para o trabalho **futuro** já descrito acima:

- Fixture P1 oficial 31×12 (bloqueia a bijeção completa do ledmap da
  bancada).
- Contrato #47-B fechado (bloqueia qualquer implementação real de rotação
  de credencial e os casos de bancada 12-15).
- Janela de bancada física coordenada com broker/backend reais (bloqueia
  todos os 15 casos da seção Bancada).

---

## Commits locais

- **Commit 1** — `test: freeze current firmware telemetry fixtures`
  — SHA `dbe01081ebecbf682443c743c9b6dd58c2d988e1`
  — fixtures novas (`event-command-received.provisional.json`,
    `event-command-accepted.provisional.json`), wiring `EMBED_TXTFILES`
    (`validation/main/CMakeLists.txt`), checks permanentes
    (`validation/main/main.cpp`).
- **Commit 2** — `docs: prepare firmware M0 recovery and bench plan`
  — este documento (`docs/M0-FIRMWARE-NOITE.md`), referenciando o SHA do
    commit 1 acima. (SHA do próprio commit 2 será informado pelo Git após
    o commit — não registrado aqui, como instruído.)

---

## Resposta final para Rodrigo/Codex

- **Worktree**: `C:\Users\Familia\Desktop\Projects\easyled-firmware-m0-prep-20261011`
- **Branch**: `chore/m0-firmware-prep-20261011`
- **Baseline**: `main` = `origin/main` = HEAD = `0dbc37e8cda71ccdaacd0ae5e896f742a5f08e36` (sem divergência)
- **Builds**: produto `idf.py -B build -D IDF_TARGET=esp32 build` — sucesso
  (`0x10f150` bytes = 1.110.352 B; DRAM 73.439 B/40,63%; IRAM 90.555 B/69,09%).
  Validação — sucesso (`0xb1a60` bytes após T1).
- **QEMU**: baseline `394/394 ALL_PASS`; após T1, `398/398 ALL_PASS`. Zero
  falhas, zero panic, zero watchdog nas duas rodadas.
- **Contagem**: 394 → 398 (+4, todos de T1).
- **Fixtures criadas/confirmadas**: 2 novas (`command_received`,
  `command_accepted`), 5 reconfirmadas byte a byte (`state`,
  `capability_reported`, `command_rejected`, `command_applied`,
  `command_failed`). SHA-256 de cada uma na tabela "Fixtures" acima.
- **Binding/geração/contadores**: confirmado — `state` e
  `capability_reported` carregam `hardware_binding_id`+`generation`;
  `state` adicionalmente carrega `boot_counter`+`state_sequence`;
  `capability_reported` não carrega contadores; os 5 ACKs não carregam
  nenhum dos três, correlacionando só por `device_id`+`command_id`.
- **`max_payload_bytes`**: confirmado como o limite do payload resolvido
  (config/preset), aplicado em três camadas independentes (buffer do
  executor, download HTTPS, parser, staging durável), nunca o envelope MQTT.
- **Mensagem MQTT excedente**: confirmado — rejeitada antes de qualquer
  extração de `command_id`; só `rejected_` sobe; nenhum ACK correlacionável
  é possível.
- **Comportamento antes do NTP**: confirmado — MQTT não inicia antes de
  `2024-01-01` no relógio do sistema; portanto nada é publicado via MQTT
  antes do NTP. Renderização da config durável não depende disso.
- **Topologia**: GPIO16/372px confirmados no perfil compilado; serpentina
  e dimensões 31×12 são inteiramente dados do payload/ledmap, não do
  firmware; nenhuma fixture oficial existe ainda (aguarda P1).
- **Perguntas para a #47-B**: 15 perguntas objetivas na seção acima.
- **Divergências**: nenhuma de comportamento; uma nuance de perfil físico
  registrada (GPIO17 presente no perfil, zero no boot seguro).
- **Impedimentos**: nenhum nesta rodada; três dependências externas
  futuras listadas (fixture P1, contrato #47-B, janela de bancada).
- **Commits locais**: commit 1 `dbe01081ebecbf682443c743c9b6dd58c2d988e1`
  (fixtures); commit 2 (este documento) — SHA informado após o commit.
- **Confirmação**: nenhum push, merge, deploy, flash, acesso serial,
  broker, backend, banco de dados, segredo ou alteração em outro
  repositório/worktree aconteceu nesta rodada. `network_credentials.hpp`
  nunca foi lido, impresso ou versionado.
