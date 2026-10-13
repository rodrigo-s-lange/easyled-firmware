# #47-B — Fundação durável do ciclo de vida de credenciais MQTT (2026-10-13)

## Resumo executivo

1. **Baseline**: branch `main`, commit `20e4049cf480e81fddec11e1e7d50dcb81462632` — exatamente o
   esperado, sem divergência. Build do produto e `validation/` confirmados limpos nesta mesma
   base antes de qualquer edição. Worktree isolada em
   `easyled-firmware-mqtt-credentials-47b/`, branch `feat/mqtt-credential-lifecycle-47b`.
2. **O que foi construído**: a fundação durável completa do ciclo `pending → active → revoked` do
   contrato #47-B — parser limitado do payload de bootstrap, resolvedor de confiança injetável,
   armazenamento durável em NVS (CRC32 + readback), cliente HTTPS de bootstrap injetável,
   máquina de recuperação determinística (coordenador), um seam mínimo no `NetworkSession` real,
   e a fiação de configuração (gitignored, fail-closed) no `main.cpp` do produto. Tudo no novo
   componente de aplicação `components/pixels_mqtt_credentials/` — nenhuma mudança em
   `esp32libfun`, Composer, efeitos ou `easyled-platform`.
3. **Validação**: novo target `validation_mqtt_credentials/`, **127/127 checks, ALL_PASS**,
   confirmado em múltiplas execuções QEMU consecutivas sem falha. O runner congelado `validation/`
   permanece **468/468 ALL_PASS**, sem nenhuma alteração de capacidade (772 B de DRAM estática
   livre, idênticos antes/depois).
4. **Integração real, não apenas fundação**: ainda não é ponta a ponta. O `main.cpp` do produto já
   constrói e mantém viva a `MqttCredentialCoordinator` de produção quando um perfil completo é
   fornecido, mas **não troca ainda** a identidade MQTT ao vivo do `NetworkSession` com base nela —
   ver "Pontos bloqueados pela plataforma" abaixo.
5. **Nenhum segredo adicionado**: nenhuma URL, token, credencial ou CA real foi embutida em
   nenhum lugar; todo material de teste é sintético e vive exclusivamente no novo target de
   validação. Ver "Confirmação de ausência de segredos".

## Procedência imutável

Referência upstream (revisável, não uma integração de produção) de
`rodrigo-s-lange/easyled-platform` @ `40e0af25a671299d71915e52b4239a005fb48296`:

- Contrato: `docs/contracts/easyled-control-contract.md`, seção "Device MQTT credential lifecycle
  (#47-B)", linhas 583-788.
- Fixture: `docs/fixtures/mqtt-provisioning/device-response.v1.json`.
- Instruções: `docs/fixtures/mqtt-provisioning/README.md`.
- Operação OpenAPI: `issueDeviceMQTTCredential`.

Importado byte a byte via `git show <SHA>:<path>` em um clone local read-only; `easyled-platform`
nunca foi tocado (sem checkout, reset, merge ou edição).

| Arquivo | Bytes | SHA-256 | Newline final |
|---|---:|---|---|
| `device-response.v1.json` | 1257 | `e92947aedc6669d44e0068103508b9c6b7a8db2a4256b987d26e02b4a29b9db2` | sim (`}\n`) |

**Primeiro fixture deste repositório que é pretty-printed** (multi-linha, indentado) em vez de
minificado — exigiu tolerância a espaço em branco à direita após o `end` do cJSON (ver "Bugs
corrigidos" abaixo); todos os fixtures anteriores eram minificados com `end == json + size` exato.

## Camada da mudança

Camada de aplicação/produto (AGENTS.md: `framework/core/` não foi tocado). Novo componente
`components/pixels_mqtt_credentials/`, seguindo exatamente o padrão já estabelecido por
`pixels_durable_storage` (mesma partição NVS `easyled`, mesmo discipline CRC32+commit+readback,
mesmo padrão de parsing cJSON com helpers privados por componente). Integração mínima com
`main.cpp` apenas onde necessário; `pixels_network`/`NetworkSession` recebeu um único seam
pontual (seção 6). `esp32libfun`, Composer e efeitos: inalterados.

## Seção 1 — Parser e fixture

`pixels_mqtt::parseDeviceCredentialResponse()` — limitado, sem heap descontrolado (cJSON sobre um
buffer fixo), valida:

- UUIDs de `device_id`/`hardware_binding_id`/`credential.id` e identidade recebida == identidade
  esperada (injetada pelo chamador, nunca lida do próprio payload).
- `generation != 0`; `credential.status == "pending"` exatamente; `credential.kind` ∈
  `{initial_provisioning, rotation}`; `credential.activation ==
  "first_successful_mqtt_authentication"` exatamente.
- `mqtt.username == mqtt.client_id == device_id`; senha não vazia, ≤128 bytes (129 rejeitado);
  `broker_uri` exclusivamente `wss://`, sem userinfo; `tls.verify_server == true` (literal);
  `tls.trust_ref` resolvido via o `TrustResolver` injetado (seção 2); 4 tópicos não vazios,
  limitados, sem `+`/`#`.
- Campos consumidos duplicados, ausentes ou com tipo errado são rejeitados; campos desconhecidos
  são ignorados (forward compatibility).
- `expires_at`: RFC3339 UTC terminando em `Z`, aceitando 0 dígitos fracionários, 1-6 dígitos
  fracionários e o exemplo real de 6 dígitos do fixture — **nunca usado para descartar uma
  credencial** quando o relógio local não é confiável (apenas metadado, nunca avaliado).

## Seção 2 — Resolvedor de confiança

`TrustResolver` (interface) + `TableTrustResolver` (tabela estática injetada). `trust_ref`
desconhecido falha fechado (`ESP_ERR_NOT_FOUND`) — nunca cai para outra CA, nunca desabilita
verificação de hostname/certificado. O fixture usa `easyled-broker-ca-v1` com material sintético
apenas no target de teste; em produção, sem uma tabela com entrada válida, o provisionamento se
recusa (ver Seção 7). **Valores reais por ambiente ainda não foram entregues pela plataforma** —
quando chegarem, a tabela real é só mais um `TableTrustResolver::Entry` injetado no perfil
gitignored do `main.cpp`, sem mudança de código.

## Seção 3 — Armazenamento durável

`MqttCredentialStore`, partição `easyled` existente (sem alterar o layout), namespace
`easyled_mqtt` (≤15 caracteres — ver "Bugs corrigidos"). CRC32 + commit + readback em cada escrita,
mesma disciplina de `pixels_durable_storage`. Estados: `Empty`, `CurrentConfirmed`,
`PendingUnused`, `AttemptInFlight`, `AmbiguousActivation`, `BootstrapRecovery`,
`ProvisioningFault`.

Regras de recuperação implementadas e testadas (reboots simulados via `end()`+`init()`,
corrupção via injeção direta de bytes na NVS):

- Nova credencial persistida (`PendingUnused`) antes de qualquer uso.
- `beginAttempt()` marca `AttemptInFlight` **antes** de entregar a credencial ao transporte.
- Reboot durante `AttemptInFlight` → `AmbiguousActivation` (nunca repetido do mesmo jeito, nunca
  assumido como falha).
- CONNECT aceito → promove a credencial em uso para `CurrentConfirmed`, remove a antiga.
- Falha antes da autenticação (`reportPreAuthFailure`) não promove nada; nenhuma credencial é
  tocada.
- Ambíguo (`reportAmbiguous`) nunca apaga a credencial em voo; conta rodadas sem sucesso.
- Corrupção (CRC/magic/schema inválidos) falha fechado — `ESP_ERR_INVALID_RESPONSE` — nunca
  fabrica uma credencial.
- `eraseAll()`: descarta ambas as credenciais e contadores, volta para `Empty`, **sem** fechar o
  handle NVS nem derrubar o estado de inicialização da partição — ver "Bugs corrigidos" sobre por
  que isso existe como método próprio em vez de reusar
  `end()`+`nvs_flash_erase_partition()`+`init()`.

## Seção 4 — Cliente HTTPS de bootstrap

`MqttBootstrapClient` (interface) + `EspMqttBootstrapClient` (produção). Base URL, hardware ID,
bearer token, CA e timeout são todos injetados — nenhuma URL/token/certificado embutido no
binário. HTTPS obrigatório, hostname/certificado verificados (bundle ESP-IDF por padrão),
redirecionamento e downgrade recusados, limite explícito de corpo, resposta só é entregue ao
parser após download completo. `Authorization` e corpo nunca logados.

Política por status (`MqttCredentialCoordinator::doBootstrap()`):

| Status | Ação |
|---|---|
| 200 | valida completo, persiste antes de usar (`storePending`) |
| 401 | `ProvisioningFault`, backoff, credenciais armazenadas preservadas |
| 403 | `ProvisioningFault`, backoff, para até novo claim/rebind externo |
| 429 | respeita `Retry-After` + jitter ±20% |
| 500 | backoff progressivo, tenta de novo |
| 503 | backoff no teto (15 min) |
| timeout/reset/sem resposta | resposta pode ter se perdido; tenta de novo após backoff |
| 404/405 | bug de firmware/config; backoff, nunca loop apertado |

## Seção 5 — Máquina de recuperação (coordenador)

`MqttCredentialCoordinator::poll()` é puramente determinístico contra interfaces fake (store real,
cliente/relógio fake) — testado em `validation_mqtt_credentials/`:

1. Sem credencial usável → `doBootstrap()`.
2. Resposta válida → persiste como `PendingUnused`.
3. Antes de conectar → marca `AttemptInFlight` duravelmente.
4. CONNECT aceito → promove a `CurrentConfirmed`.
5. Resultado ambíguo → `AmbiguousActivation`.
6. Em ambiguidade, a credencial nova é tentada primeiro a cada rodada
   (`pending_tried_this_round_`, transiente, nunca persistido — seguro resetar num reboot real).
7. A antiga só é tentada depois, como cortesia, e só uma vez por rodada.
8. Após 3 rodadas (configurável) sem autenticação de nenhuma das duas → `BootstrapRecovery`.
9. Nunca tight-loop: todo caminho sem sucesso agenda `scheduleNextAttempt()` antes de retornar
   `Idle`.
10. Backoff: início 5 s, dobra progressivamente, teto 15 min, jitter ±20%; `Retry-After`
    prevalece quando recebido. Relógio monotônico injetado (`MonotonicClock`) — nunca depende de
    NTP para corretude.

## Seção 6 — Integração com `NetworkSession` (seam mínimo)

Analisado o código real do ESP-MQTT (`managed_components/espressif__mqtt/include/mqtt_client.h`)
antes de qualquer mudança. `esp_mqtt_error_type_t` distingue `MQTT_ERROR_TYPE_TCP_TRANSPORT`
(aliased por `MQTT_ERROR_TYPE_ESP_TLS`) de todo o resto. Único ponto provável como falha
pré-autenticação: a TCP/TLS handshake falhou antes de qualquer CONNECT chegar ao broker. Tudo o
mais (CONNACK perdido, `CONNECTION_REFUSED` — que carrega um código de rejeição do próprio broker
e pode seguir uma autenticação bem-sucedida —, desconexão posterior) permanece ambíguo por
desenho.

**Mudança real, a menor possível**: um novo contador atômico
`Status::pre_auth_transport_errors`, incrementado apenas dentro do `case MQTT_EVENT_ERROR:` já
existente, apenas quando `error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT`. Nenhuma
reescrita do `NetworkSession`; `transport_errors` (contador já existente) continua incrementando
exatamente como antes — `pre_auth_transport_errors` é um subconjunto adicional, nunca substitui
nada. Senha nunca exposta em status/eventos/logs (inalterado). Confirmado sem regressão: o runner
congelado `validation/` permanece 468/468 ALL_PASS, DRAM estática idêntica (772 B livres, antes e
depois).

Não implementado nesta rodada, de propósito: a troca da identidade/credencial ao vivo do
`NetworkSession` com base no coordenador (chamar `end()` antes de mudar credenciais, manter
buffers referenciados vivos durante a sessão). Essa é exatamente a peça que depende de valores
reais de `trust_ref`/endpoint/credencial para ser validada com segurança — ver "Pontos bloqueados
pela plataforma".

## Seção 7 — Integração em `main.cpp`

Modo laboratório atual **preservado exatamente**: com `mqtt_provisioning_enabled == false` (padrão,
e todo perfil até a plataforma entregar valores reais), `network.username`/`password`/
`broker_uri` continuam alimentando a conexão literalmente, igual a antes desta feature existir —
nenhum endpoint de bootstrap é contatado.

Com `mqtt_provisioning_enabled == true`:

- **Perfil incompleto** (falta `mqtt_bootstrap_base_url`/`mqtt_bootstrap_token`/`mqtt_trust_ref`/
  `mqtt_trust_ca_pem`): recusa iniciar rede inteiramente — nunca cai silenciosamente para as
  credenciais de laboratório que `network.*` também possa conter. Espelha exatamente o tratamento
  já existente de `HttpsProfileState::Incomplete`.
- **Perfil completo**: constrói `MqttCredentialStore`/`EspMqttBootstrapClient`/
  `TableTrustResolver`/`EspMonotonicClock`/`MqttCredentialCoordinator` de produção, chama
  `store.init()`. Se falhar, loga e o provisionamento fica ocioso nesse boot (laboratório
  inalterado). Se OK, `mqtt_coordinator.poll()` é chamado a cada iteração do loop principal,
  mantendo o bootstrap/backoff vivo e observável via log — **mas sem ainda dirigir a sessão MQTT
  real** (ver Seção 6). `session.init(config.network)` continua usando os campos estáticos de
  `config.network`, sem mudança.

Como nenhum perfil real existe hoje (`network_credentials.hpp` é gitignored e nunca foi
commitado), `mqtt_provisioning_enabled` é `false` em todo boot real até agora — o código novo
nunca executa fora do target de validação.

## Seção 8 — Novo target de validação

`validation_mqtt_credentials/` (target `easyled_mqtt_credentials_validation`), isolado do runner
congelado `validation/` — nenhuma linha daquele arquivo foi tocada, nenhum fixture novo foi
adicionado lá. HTTP e MQTT inteiramente fake; nenhum broker ou endpoint real.

**127/127 checks, ALL_PASS**, confirmado em 4 execuções QEMU consecutivas sem falha (ver
"Memória e desempenho"). Cobertura: fixture exato + SHA-256; parser caminho feliz; `expires_at`
com 0, 1 e 6 dígitos fracionários; timestamps inválidos; campos ausentes/duplicados/tipo errado;
senha 128 aceita / 129 rejeitada; `broker_uri` não-WSS rejeitado antes de persistir;
`verify_server:false` rejeitado; `trust_ref` desconhecido rejeitado; username/client_id
divergente rejeitado; tópicos com wildcard rejeitados; persist-before-use; reboot em cada
transição, incluindo durante `AttemptInFlight`; CONNACK perdido; autenticação aceita mas
confirmação local perdida; credencial nova tentada primeiro, antiga só como cortesia; três
rodadas → `BootstrapRecovery`; 401/403/429/500/503/404/405; timeout/resposta perdida;
`Retry-After`; corrupção NVS; nenhum segredo em logs/status; renderização independente do
coordenador em backoff (não exercida ativamente por não haver pixels neste target, mas garantida
por desenho — este componente nunca toca pixels/Composer).

## Bugs corrigidos durante a validação

Encontrados e corrigidos nesta mesma rodada, todos confirmados via re-execução QEMU após cada
correção:

1. **Stack overflow → corrupção de heap**: `CONFIG_ESP_MAIN_TASK_STACK_SIZE` padrão (8192) não
   bastava para a recursão do cJSON somada ao frame grande de `app_main`. Diagnosticado via
   `addr2line` no backtrace do panic (`LoadProhibited` dentro do alocador) comparado ao endereço
   base do heap do boot log. Corrigido para 24576 em `sdkconfig.defaults`.
2. **Parser rejeitando o próprio fixture real**: o fixture é o primeiro pretty-printed do
   repositório, terminando em `}\n`; a checagem pós-parse original exigia `end == json + size`
   exato, que não tolera a nova linha final do cJSON. Corrigido com um helper
   `onlyTrailingWhitespace()`.
3. **CRC computado mas nunca verificado**: um aviso do compilador revelou que `crc32()` era
   calculado mas nunca consultado. Adicionado `crc` ao final de `WireHeader`/`WireCredential`,
   selado antes de escrever e verificado antes de aceitar no `load()`.
4. **`ConnectWithCurrentCourtesy` inalcançável**: `credentialToTry()` só sabia responder para
   `AttemptInFlight`; chamado entre `poll()` retornar a ação e `beginAttempt()` ainda não ter sido
   chamado, caía sempre em `pending()`. Corrigido replicando a mesma lógica de decisão do
   `poll()` para o estado "ainda não comprometido".
5. **Namespace NVS com 16 caracteres**: `"easyled_mqttcred"` excede
   `NVS_KEY_NAME_MAX_SIZE-1` (15). Retornava `ESP_ERR_NVS_KEY_TOO_LONG`, mascarado por outro erro
   anterior (ver próximo item) até este ser corrigido primeiro. Renomeado para
   `"easyled_mqtt"` (12 caracteres).
6. **`nvs_flash_init_partition("easyled")` nunca chamado**: nenhum lugar do código inicializava a
   partição antes de abri-la — `nvs_open_from_partition()` retornava
   `ESP_ERR_NVS_NOT_INITIALIZED`. Corrigido chamando `nvs_flash_init_partition()` dentro do próprio
   `MqttCredentialStore::init()`, mesmo padrão já usado por `pixels_durable_storage`.
7. **Handle NVS aberto sobrevivendo a um `nvs_flash_erase_partition()`**: uma seção de teste
   erasava a partição sem jamais ter chamado `store.end()` antes, apagando a partição sob um
   handle ainda aberto — causa raiz de um crash intermitente por divisão por zero (`EXCCAUSE=0x6`)
   dentro do próprio scanner de páginas da NVS (`nvs::PageManager::load`), resolvido via
   `addr2line`. Corrigido adicionando o `end()` ausente.
8. **Ciclo `end()`+`nvs_flash_erase_partition()`+`init()` repetido demais em sequência**: mesmo
   com o handle corretamente fechado, repetir esse ciclo caro (reinicialização completa do driver
   NVS da partição) 6 vezes em sequência dentro de um único boot QEMU continuava instável sob
   timing. Resolvido criando `MqttCredentialStore::eraseAll()` — reseta os próprios registros no
   lugar, sem nunca tocar o estado de inicialização da partição — reservando o ciclo caro
   exclusivamente para os testes que de fato simulam um reboot real.

Todos os 8 itens foram bugs reais de implementação (parser, store, coordenador ou do próprio
teste), nunca do contrato ou do fixture upstream.

## Memória e desempenho

| | `validation/` (congelado) | `validation_mqtt_credentials/` (novo) | Produto |
|---|---|---|---|
| Checks QEMU | 468/468 ALL_PASS (inalterado) | 127/127 ALL_PASS | n/a |
| DRAM estática | 179964 B (99,57%, **772 B livres**, inalterado) | 39836 B (22,04%, 140900 B livres) | 81063 B (44,85%, antes 73439 B/40,63%) |
| IRAM estática | 48047 B (36,66%, inalterado) | 46163 B (35,22%) | 90555 B (69,09%, inalterado) |
| Flash code+data | inalterado | 147278 B | 997574 B (código+dados atuais; não medi o valor "antes" desta rodada para flash, só DRAM/IRAM) |

O aumento de DRAM do produto (+7624 B, de 73439 B/40,63% medido na rodada P1 anterior) vem dos
novos objetos estáticos em `app_main()`
(`MqttCredentialStore` com seu `record_[2048]` de scratch, `MqttCredentialCoordinator` com seu
`response_buffer_[2048]`) — construídos sempre, mesmo em modo laboratório, porque `main.cpp` os
declara incondicionalmente (só o `store.init()` é condicional ao perfil). Headroom do produto
permanece confortável (99673 B livres). O runner congelado não tem margem nenhuma para crescer
(772 B) e não foi tocado.

A suíte nova do `validation_mqtt_credentials/` demandou, em uma execução QEMU completa e correta,
algo entre 90 s e 300 s de tempo real de parede (não apenas tempo simulado) — significativamente
mais que os runners anteriores deste repositório. Isso não é uma regressão desta feature por si
só, mas uma característica do volume de operações NVS reais (cada transição de estado grava 3
blobs com CRC + commit + readback) somado à emulação fiel de timing de flash do QEMU; runners
futuros com este mesmo padrão de uso intensivo de NVS devem esperar o mesmo. Nenhuma correção foi
tentada para isso além de reduzir ciclos de erase+reinit redundantes (item 8 acima).

## Pontos ainda bloqueados pela plataforma

Registrados para resposta à equipe de plataforma; até essas respostas chegarem, a implementação
permanece injetável, testável e fail-closed:

1. Valores reais de `trust_ref` e respectivas CAs por ambiente.
2. Base URL do endpoint de bootstrap implantado.
3. Mecanismo real de entrega do bearer token de bootstrap.
4. Limite de taxa por ambiente (embora `Retry-After` já seja suficiente do lado do firmware).
5. Confirmação/merge definitivo do contrato #47-B.

Consequência direta no firmware: a troca da identidade MQTT ao vivo do `NetworkSession` com base
no coordenador (Seção 6/7) permanece pronta em código mas não fiada ao caminho real — não há como
validar esse último passo com segurança sem pelo menos um valor real de cada um dos itens acima.

## Confirmação de ausência de segredos

Nenhuma URL, bearer token, credencial MQTT ou material de CA real foi adicionado em nenhum commit
desta rodada. Toda senha/token/CA no novo target de validação é explicitamente sintética
(`SYNTHETIC-NOT-A-CREDENTIAL-...`, `bootstrap-test-token`, `-----BEGIN CERTIFICATE-----
\nSYNTHETIC-TEST-ONLY...`) e vive exclusivamente em `validation_mqtt_credentials/`. O diff dos
arquivos de produção (`main/main.cpp`, `components/pixels_network/*`) só referencia *nomes* de
campo (`mqtt_bootstrap_token`, por exemplo) — todos inicializados em `nullptr`, nunca atribuídos a
um valor literal. `git diff --check` limpo (sem erro de whitespace). Diretórios `build*/` e
`sdkconfig` gerados ficaram de fora do commit (gitignore do novo target corrigido para seguir o
mesmo padrão já usado por `validation/`).
