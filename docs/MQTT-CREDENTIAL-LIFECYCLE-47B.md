# #47-B — Fundação durável do ciclo de vida de credenciais MQTT (2026-10-14)

## Resumo executivo

1. **Baseline desta rodada corretiva**: branch `feat/mqtt-credential-lifecycle-47b`, HEAD inicial
   `728009c` (os três commits da rodada anterior, preservados). Working tree limpa antes de
   editar. Nenhuma mudança em `easyled-platform`, `esp32libfun` ou nas bibliotecas de pixels.
2. **O que esta rodada corrigiu**: a revisão do Codex sobre a rodada anterior encontrou 8
   bloqueadores reais de correção/segurança/design, todos corrigidos nesta rodada única e coesa
   (detalhe item a item em "Rodada corretiva" abaixo):
   `hardware_id` nunca mais confundido com `hardware_binding_id`; `generation` agora é um
   gate de autoridade exato no parser; o armazenamento passou de 3 blobs separados (não atômico)
   para um registro A/B autocontido com fault injection determinística; falhas duráveis do store
   agora se propagam como `esp_err_t` até o coordenador, que não avança bookkeeping quando elas
   ocorrem; a classificação "pré-autenticação" do `NetworkSession` foi **removida** (era falsa:
   `MQTT_ERROR_TYPE_TCP_TRANSPORT` também ocorre esperando CONNACK, depois do CONNECT já enviado);
   a definição de "rodada" agora é pending-depois-cortesia completos, não tentativas individuais;
   emissão de credencial real que o produto ignorava foi substituída por um controlador de sessão
   testável que **não** é ligado à sessão real ainda (decisão explícita, justificada abaixo); e o
   runner `run_qemu.py` agora reconhece o marcador exato deste target e retorna 0/1 corretamente.
3. **Validação**: novo target `validation_mqtt_credentials/` — **298/298 checks, ALL_PASS**
   no commit de implementação `51817bf`, confirmado pelo `run_qemu.py` com exit code 0
   (o próprio runner, corrigido nesta rodada, agora confirma isso — não apenas minha leitura do
   transcript). O runner congelado `validation/` permanece **468/468 ALL_PASS**, exit 0, sem
   nenhuma alteração de capacidade (772 B de DRAM estática livre, idênticos antes/depois). Build do
   produto confirmado limpo.
4. **Integração real, ainda não ponta a ponta — por decisão explícita, não por omissão**: o
   `MqttSessionController` (novo, testável) prova a orquestração completa contra um transporte
   MQTT fake, mas **não é instanciado no `main.cpp` do produto**. Motivo técnico concreto,
   encontrado ao reanalisar `NetworkSession::init()` nesta rodada: ele valida e cria o cliente
   ESP-MQTT **antes** de subir o Wi-Fi, exigindo uma identidade MQTT completa só para começar —
   não há como "começar só com Wi-Fi, decidir a credencial depois" sem resolver essa ordenação em
   `NetworkSession`, o que é uma questão de design daquele componente, não deste ciclo de vida de
   credencial. Ver "Seção 7" e "Pontos bloqueados pela plataforma".
5. **Nenhum segredo adicionado**: confirmado de novo nesta rodada — ver "Confirmação de ausência
   de segredos".

## Procedência imutável

Inalterada desta rodada; revalidada por releitura antes de editar. Referência upstream
(revisável, não uma integração de produção) de `rodrigo-s-lange/easyled-platform` @
`40e0af25a671299d71915e52b4239a005fb48296`:

- Contrato: `docs/contracts/easyled-control-contract.md`, seção "Device MQTT credential lifecycle
  (#47-B)", linhas 583-788.
- Fixture: `docs/fixtures/mqtt-provisioning/device-response.v1.json`.
- Instruções: `docs/fixtures/mqtt-provisioning/README.md`.
- OpenAPI: operação `issueDeviceMQTTCredential`; schemas `ClaimResponse`/`ClaimabilityResponse`
  (padrão `hardware_id`); parâmetro de path `hardware_id` do próprio
  `/device-bootstrap/{hardware_id}/mqtt-credential`.

| Arquivo | Bytes | SHA-256 | Newline final |
|---|---:|---|---|
| `device-response.v1.json` | 1257 | `e92947aedc6669d44e0068103508b9c6b7a8db2a4256b987d26e02b4a29b9db2` | sim (`}\n`) |

## Camada da mudança

Inalterada: camada de aplicação/produto. Mudanças desta rodada confinadas a
`components/pixels_mqtt_credentials/` (a maior parte), `components/pixels_network/` (reversão do
seam da rodada anterior) e `main/main.cpp`. Nenhuma mudança em `esp32libfun`, Composer, efeitos ou
`easyled-platform`.

## Rodada corretiva: os 8 achados e o que foi feito

### 1. `hardware_id` separado de `hardware_binding_id`

O path `POST /api/v1/device-bootstrap/{hardware_id}/mqtt-credential` usa o identificador físico
da unidade (`^[0-9a-f]{1,64}$`, OpenAPI), nunca o UUID `hardware_binding_id` do corpo da resposta
(que muda a cada rebind; o `hardware_id` nunca muda). A rodada anterior os confundia: o
`main.cpp` passava `config.hardware_binding_id` (um UUID) como `hardware_id` do coordenador, e
`EspMqttBootstrapClient::requestCredential()` validava esse campo com a função `uuid()` —
exatamente invertido: um UUID real teria *passado* essa checagem, e um `hardware_id` lower-hex
real (sem hifens) teria sido *rejeitado*.

Corrigido:
- Nova função exportada `pixels_mqtt::isValidHardwareId()` — lower-hex, 1 a 64 caracteres, o
  padrão exato do OpenAPI. Testada exaustivamente (vazio, nulo, maiúsculas, caractere fora de
  `[0-9a-f]`, 64 e 65 caracteres como limite).
  `EspMqttBootstrapClient::requestCredential()` agora usa `isValidHardwareId()`, não `uuid()`.
- Novo campo `ApplicationConfig::mqtt_hardware_id` em `main.cpp`, documentado como distinto de
  `hardware_binding_id`; `mqttProvisioningProfileState()` valida seu formato antes de aceitar o
  perfil como completo.
- `main.cpp` passa `config.mqtt_hardware_id` (nunca `hardware_binding_id`) para
  `mqtt_coordinator_config.hardware_id`.
- `hardware_binding_id` continua sendo o UUID esperado no corpo da resposta; nunca derivado de
  `hardware_id` nem o contrário.
- Teste dedicado: a credencial do coordenador contra um `FakeBootstrapClient` confirma
  `last_request.hardware_id == kHardwareId` e `!= kHardwareBindingId`.

### 2. `generation` como token de autoridade exato

`parseDeviceCredentialResponse()` ganhou um parâmetro `expected_generation`. A verificação ocorre
**antes** de `trust_ref` ser resolvido (que só acontece depois do parser retornar `ESP_OK`, em
`doBootstrap()`) e antes de qualquer persistência ou conexão. `generation == 0` do lado da
resposta já era rejeitado (`Malformed`, checagem pré-existente); agora, adicionalmente,
`expected_generation == 0` (defensivo, nunca deveria ocorrer para uma binding ativa) e
`generation != expected_generation` são rejeitados como `IdentityMismatch` — geração anterior
*e* posterior igualmente, nunca uma comparação de ordem (OpenAPI `ClaimResponse.generation`: "an
exact-match authority token... never an ordering comparison").

Testes: geração correta aceita; geração 0 da resposta rejeitada (`Malformed`); geração anterior
(`kGeneration - 1`) e posterior (`kGeneration + 1`) ambas rejeitadas (`IdentityMismatch`);
`expected_generation == 0` do lado do chamador também rejeitado. `main.cpp` passa
`config.generation` para `mqtt_coordinator_config.expected_generation`, e
`mqttProvisioningProfileState()` exige `config.generation != 0` para considerar o perfil completo.

### 3. Armazenamento genuinamente atômico (registro A/B)

O esquema anterior gravava `hdr`/`current`/`pending` como **três blobs separados** em três
commits distintos — uma queda de energia entre eles podia deixar o registro misturado (ex.:
`state` novo com credenciais antigas). Substituído por exatamente o que foi pedido: dois slots
NVS autocontidos (`slotA`/`slotB`), cada um com schema, `sequence` (u32 monotônico), estado, flag
de tentativa, contador de rodadas, a credencial `current` completa, a `pending` completa e um
CRC32 sobre tudo — uma única `nvs_set_blob()` + `nvs_commit()` + readback por transição, sempre
no slot que **não** está ativo. RAM só é atualizada depois dessa única escrita verificada; uma
falha em qualquer ponto dela deixa RAM e o slot ativo exatamente como estavam — não há restauração
porque nada foi mutado antecipadamente. No boot, os dois slots são validados de forma
independente (magic/schema/CRC, depois invariantes estado↔credencial) e o válido com maior
`sequence` vence; se só um é válido, esse vence incondicionalmente; se nenhum é, falha fechado.
**Nenhum ponteiro de "slot ativo" separado existe** — isso recriaria a mesma janela não atômica um
nível acima.

Formato novo (`kSchema = 2`); como o formato antigo nunca foi lançado, não há migração.

Invariantes verificadas ao carregar (violação é tratada como corrupção, igual a falha de CRC):
`Empty` exige nem current nem pending válidos; `CurrentConfirmed` exige current válido e pending
**inválido**; `PendingUnused` exige pending válido; `AttemptInFlight`/`AmbiguousActivation`
exigem válido o lado que a flag de tentativa aponta; `BootstrapRecovery`/`ProvisioningFault` não
têm exigência estrita.

**Fault injection determinística** (`StoreTestFault`, método de teste
`setFaultInjectionForTest()`, nunca chamado em produção): `BeforeWrite` (a escrita nunca é
tentada), `DuringWrite` (como se `nvs_set_blob()` falhasse), `DuringCommit` (grava e comita
*lixo* real no slot candidato em vez de pular o commit — ver nota abaixo), `DuringReadback`
(escrita e commit reais bem-sucedidos, mas a comparação de readback é forçada a reportar
divergência mesmo assim). Testado: as quatro falhas, reboot depois de cada uma, recuperação
sempre pelo slot anterior (exceto `DuringReadback`, cujo dado novo realmente está durável — um
reboot real recupera corretamente o slot *novo*, provando que a verificação conservadora não
causa perda permanente); nunca `PendingUnused` sem pending válida; nunca `CurrentConfirmed` sem
current válida (`beginAttempt()`+`confirmConnected()` sob falha de commit, RAM fica em
`AttemptInFlight`, reboot reconcilia para `AmbiguousActivation`, nunca mostra `CurrentConfirmed`
com dado obsoleto).

*Nota de implementação encontrada durante a validação*: simular "falha no commit" apenas
pulando `nvs_commit()` depois de um `nvs_set_blob()` real não bastava — a NVS pode manter uma
escrita não comitada visível a um fechar+reabrir de handle no mesmo processo (o `end()`+`init()`
que estes testes usam para simular reboot), mesmo que uma queda de energia real a tivesse
perdido. Corrigido escrevendo e comitando *lixo* de verdade no slot candidato nesse caso, para que
ele seja inequivocamente inválido em qualquer `load()` subsequente, independente desse detalhe de
visibilidade.

`eraseAll()` (já existente da rodada anterior) continua: descarta ambas as credenciais e
contadores, volta para `Empty`, sem fechar o handle NVS nem tocar o estado de inicialização da
partição.

### 4. Falhas duráveis propagadas

`MqttCredentialStore::reportAmbiguous()` ganhou um parâmetro `completesRound` (ver item 6) mas
seu retorno já era `esp_err_t`. A mudança real foi no **coordenador**: `reportConnected()`,
`reportPreAuthFailure()` e `reportAmbiguous()` agora retornam `esp_err_t` (eram `void`,
engolindo o resultado do store). Quando a gravação durável falha, o coordenador **não** avança
`roundsWithoutSuccess`, **não** limpa `pending_tried_this_round_` e **não** assume nenhuma
transição de estado — apenas agenda o próximo backoff (para nunca tight-loop) e retorna o erro.
`enterBootstrapRecovery()`/`enterProvisioningFault()` (chamados internamente pelo coordenador)
também têm seu retorno verificado onde chamados.

Teste dedicado ("durable-failure propagation"): fault injection (`DuringCommit`) em
`reportAmbiguous()` e em `reportConnected()`, confirmando que a primeira chamada falha e nada
avança (`roundsWithoutSuccess` continua 0, estado continua `AttemptInFlight`), e que a chamada
seguinte (falha automaticamente limpa) progride normalmente.

### 5. Classificação pré-autenticação removida (era falsa)

A leitura do `mqtt_client.c` vendorizado (`managed_components/espressif__mqtt/`) confirmou o
achado do Codex: `esp_mqtt_client_dispatch_transport_error()` (que define
`MQTT_ERROR_TYPE_TCP_TRANSPORT`) é chamada de dentro de `transport_read()`'s caminho de erro
genérico, usado tanto durante o connect inicial quanto durante `mqtt_message_receive()` —
exatamente o caminho que espera o CONNACK, **depois** do CONNECT já ter sido enviado. O tipo do
erro, sozinho, não prova nada sobre a ordem relativa à autenticação.

Removido inteiramente: o contador `pre_auth_transport_errors` e sua lógica em
`NetworkSession::Status`/`network_session.cpp` (introduzidos na rodada anterior) foram revertidos
— `network_session.hpp`/`.cpp` voltaram ao comportamento anterior a esta feature,
`transport_errors` continua incrementando exatamente como sempre. O `case MQTT_EVENT_ERROR:` tem
apenas um comentário novo explicando por quê nenhuma classificação adicional é segura.

Postura adotada daqui para frente: qualquer falha entre `beginAttempt()` e um `MQTT_EVENT_CONNECTED`
real é ambígua, sem exceção. Isso é reforçado por **construção de tipo**, não só por lógica: o novo
`MqttTransportSession::Outcome` (seção 7) tem exatamente três valores —
`StillTrying`/`Connected`/`Ambiguous` — e **nenhum valor de "pré-auth comprovado"**, então nenhuma
implementação desse seam (real ou fake) consegue reportar essa categoria mesmo que tentasse. O
teste dedicado explícito (`MqttSessionController`, "o ponto de revisão explícito") simula um erro
de transporte imediatamente depois de `restart()` — equivalente a um `TCP_TRANSPORT` recebido
esperando CONNACK — e confirma que o resultado é sempre `AmbiguousActivation`, nunca outra coisa.
`MqttCredentialStore::reportPreAuthFailure()` continua existindo na API (para uma futura evidência
genuinamente inequívoca, se um dia existir) mas **nada nesta base de código a chama hoje**.

### 6. Definição de rodada corrigida

Uma rodada é "pending primeiro, current como cortesia depois", **quando `current` existe**.
`roundsWithoutSuccess` só avança quando a rodada inteira termina: se há `current` para tentar como
cortesia, a tentativa contra `pending` **não** completa a rodada (só marca
`pending_tried_this_round_`); a tentativa de cortesia contra `current` sempre completa. Sem
`current`, uma única tentativa ambígua contra `pending` já completa a rodada. `BootstrapRecovery`
só é solicitado depois de 3 rodadas **completas**, nunca 3 tentativas individuais (com `current`
presente, isso são 6 tentativas; sem `current`, são 3).

Implementado via `MqttCredentialStore::reportAmbiguous(bool completesRound)` — o store só grava o
que o coordenador decide atomicamente junto da transição; `MqttCredentialCoordinator::reportAmbiguous()`
calcula `completesRound = !was_pending || !store_.hasCurrent()`.

Dois testes dedicados: (a) com `current` presente (herdado da seção "cortesia" já existente),
cada rodada agora exige 2 tentativas, confirmado round a round; (b) novo, "somente pending" (sem
`current` nunca promovido), cada rodada é 1 tentativa só, confirmado round a round até
`BootstrapRecovery` em exatamente 3 tentativas.

### 7. Controlador de sessão: construído e testado, não ligado à sessão real

Implementado exatamente como preferido: `MqttTransportSession` (interface mínima —
`restart(identity)`/`end()`/`poll() -> Outcome`, sem valor de pré-auth, ver item 5) +
`MqttSessionController` (consome `CoordinatorAction`, chama `beginAttempt()` duravelmente *antes*
de tocar o transporte, resolve `trust_ref` para a CA, monta `MqttSessionIdentity` a partir da
`StoredCredential`, chama `restart()`, amostra `poll()` e reporta `Connected`/`Ambiguous` de volta
ao coordenador de forma conservadora — nunca assume sucesso de uma chamada que falhou). Testado
de ponta a ponta contra um `FakeTransportSession` (nunca uma sessão real): credencial escolhida
corresponde à decisão do coordenador, rotação troca a identidade corretamente, ambiguidade não
descarta a credencial nova, "nova primeiro, antiga só como cortesia" confirmado através do
controlador, e falha de `restart()` em si é reportada como ambígua (nunca uma categoria especial).

**Decisão explícita de não ligar ao `main.cpp` real nesta rodada**, pela razão técnica concreta
encontrada ao reanalisar `NetworkSession::init()` (`components/pixels_network/network_session.cpp`):
ele valida `broker_uri`/`client_id`/`username`/`password`/`command_topic` e cria o cliente
ESP-MQTT **imediatamente**, antes de `start()` subir o Wi-Fi — não há como chamar `init()` só com
Wi-Fi e "decidir a credencial MQTT depois", e não há como pedir um bootstrap HTTPS sem
conectividade já estabelecida. Resolver essa ordenação exigiria ou (a) uma reescrita não-trivial
de `NetworkSession` para separar Wi-Fi de identidade MQTT (explicitamente fora de escopo: a seção
6 original já proibia "grande reescrita, apenas o menor seam necessário"), ou (b) um caminho de
Wi-Fi-somente paralelo só para o bootstrap inicial (plumbing novo, não solicitado e arriscado de
construir sem endpoint real para validar contra). Com o fallback explícito já previsto pela
tarefa ("se concluir essa integração sem endpoint real não for possível... mantenha o componente
offline"), a escolha foi: não chamar `poll()`/`tick()` no `main.cpp` do produto, não emitir
nenhuma credencial real. `main.cpp` ainda constrói `MqttCredentialStore`/`MqttCredentialCoordinator`
de produção e chama `store.init()` contra a partição real (prova que a camada durável funciona),
mas nunca avança daí. O `MqttSessionController`/`FakeTransportSession` em si não dependem de
`pixels_network` — o adaptador concreto sobre `NetworkSession` real é o próximo passo, depois que
a questão de ordenação Wi-Fi/identidade for resolvida e os valores reais existirem.

Modo laboratório **inalterado**: com `mqtt_provisioning_enabled == false` (padrão e único valor em
qualquer perfil real hoje, já que `network_credentials.hpp` nunca foi commitado),
`network.username`/`password`/`broker_uri` continuam alimentando a conexão literalmente. Perfil
incompleto (`mqtt_provisioning_enabled == true` mas faltando `mqtt_hardware_id` válido,
`mqtt_bootstrap_base_url`, `mqtt_bootstrap_token`, `mqtt_trust_ref`, `mqtt_trust_ca_pem` ou
`generation == 0`) recusa iniciar rede inteiramente, nunca cai para as credenciais de laboratório.

### 8. `run_qemu.py` corrigido

O regex de sucesso herdado do `validation/` original procurava
`NETWORK(?:_CONTRACT)?_VALIDATION` — nunca batia com o marcador real deste target,
`MQTT_CREDENTIALS_VALIDATION`. Na prática, isso significava que `main()` sempre retornava 1,
mesmo num `ALL_PASS` genuíno (eu estava lendo o transcript manualmente, nunca confiando no
exit code). Corrigido para `r"MQTT_CREDENTIALS_VALIDATION checks=\d+ failures=0 RESULT=ALL_PASS"`;
o padrão de falha (`Guru Meditation|assert failed|FAILED check|RESULT=FAIL|task_wdt`) já estava
correto e não precisou mudar. Confirmado nesta rodada: 3 execuções consecutivas, todas com
`run_qemu.py` retornando exit code 0.

## Bug adicional encontrado e corrigido durante a validação desta rodada

**Corrupção da seção "corrupção falha fechado" se propagando para as seções seguintes**: o teste
que prova que o store falha fechado contra corrupção grava lixo nos dois slots de propósito (isso
é o teste) e nunca limpa depois — correto para o que aquele teste prova, mas deixava a partição
permanentemente inutilizável para todas as seções seguintes dentro da mesma execução, causando
uma cascata de falhas e eventualmente um crash (`Guru Meditation: LoadProhibited`) numa seção bem
mais adiante. Diagnosticado por eliminação sistemática: tentei primeiro aumentar o stack
(hipótese errada, descartada por teste direto), depois instrumentação passo a passo isolando
`parse`/`trust.resolve()`/`storePending()` individualmente (todos corretos), até capturar o
retorno real de `store.init()` (`ESP_ERR_INVALID_RESPONSE`) e, por fim, imprimir diretamente de
dentro de `load()` qual verificação rejeitava cada slot — revelando os 4 bytes de lixo do teste de
corrupção anterior em ambos. Corrigido adicionando um `nvs_flash_erase_partition("easyled")` real
imediatamente depois daquele teste (o único lugar nesta rodada onde o ciclo caro de
erase+reinit de partição ainda é necessário, já que não existe um store inicializado ali para usar
`eraseAll()`). Toda a instrumentação de depuração foi removida depois de confirmado o fix.

## Memória e desempenho

| | `validation/` (congelado) | `validation_mqtt_credentials/` | Produto |
|---|---|---|---|
| Checks QEMU | 468/468 ALL_PASS (inalterado) | 298/298 ALL_PASS no `51817bf` | n/a |
| Exit code `run_qemu.py` | 0 (inalterado) | 0 (corrigido nesta rodada — antes sempre 1, item 8) | n/a |
| DRAM estática | 179964 B (99,57%, **772 B livres**, inalterado) | 90300 B (49,96%, **90436 B livres**) | 82111 B (45,43%, antes 81063 B/44,85%) |
| IRAM estática | 48047 B (36,66%, inalterado) | 46163 B (35,22%, inalterado) | 90555 B (69,09%, inalterado) |
| Flash code | inalterado | 108430 B | 772014 B |
| Flash data | inalterado | 44816 B | 220600 B |

`CONFIG_ESP_MAIN_TASK_STACK_SIZE` do novo target foi elevado de 24576 para 40960 — a suíte de
testes cresceu substancialmente (registro A/B maior que o esquema de 3 blobs anterior, mais
seções) e a margem extra é barata dado o headroom confortável de DRAM (45%). O aumento de stack
*não* era a causa do bug real encontrado nesta rodada (confirmado por teste direto antes de
localizar a causa raiz verdadeira), mas foi mantido como margem de segurança razoável.

## Pontos ainda bloqueados pela plataforma

Estritamente dependências externas da equipe de plataforma; até essas respostas chegarem, a
implementação permanece injetável, testável e fail-closed:

1. Valores reais de `trust_ref` e respectivas CAs por ambiente.
2. Base URL do endpoint de bootstrap implantado.
3. Mecanismo real de entrega do bearer token de bootstrap.
4. Limite de taxa por ambiente (embora `Retry-After` já seja suficiente do lado do firmware).
5. Confirmação/merge definitivo do contrato #47-B.

## Próxima tarefa de arquitetura do firmware (não bloqueada pela plataforma)

A ordenação Wi-Fi-antes-de-identidade-MQTT descrita na Seção 7 é uma decisão de design interna de
`NetworkSession`, não uma dependência externa — não precisa esperar nenhuma resposta da
plataforma para ser resolvida, e pode (e deve) ser atacada independentemente dos 5 itens acima.
Só depois dela resolvida é que a ligação do `MqttSessionController` a um `NetworkSession` real
tem onde pousar; os valores reais de `trust_ref`/URL/token continuam sendo necessários antes de
qualquer teste de ponta a ponta contra um ambiente real, mas a arquitetura em si não depende deles.

## Rodada corretiva 2: correção final pequena sobre `a950abf`

Cinco correções pontuais, sem reabrir parser, formato A/B, `hardware_id`, `generation`,
contagem de rodadas ou classificação pré-auth (todos intocados desta vez):

1. **`MqttSessionController` perdia o resultado durável em falha**: `tick()` limpava
   `attempt_active_` antes de saber se `reportConnected()`/`reportAmbiguous()` conseguiram
   persistir; em falha, o store ficava em `AttemptInFlight` e o controlador entrava em `Idle`
   permanentemente (nunca mais reportava nada, nunca mais tentava de novo). Corrigido com um
   estado explícito `PendingResult{None, Connected, Ambiguous}`: ao observar um resultado
   terminal do transporte (ou uma falha imediata de `restart()`), o resultado é guardado em
   `pending_result_` e `reportPendingResult()` é quem tenta persisti-lo; enquanto isso falhar,
   `tick()` **só** tenta esse mesmo report de novo — nunca consulta `transport_.poll()` nem chama
   `transport_.restart()` nesse meio tempo. `ready_` só vira `true` dentro de
   `reportPendingResult()`, e só depois de `reportConnected()` retornar `ESP_OK`; fica
   inalterado em qualquer tick ocioso (sem resultado pendente, sem tentativa ativa) depois disso.
   `ready()` documentado com precisão: não é um pulso de um tick, é "o último resultado
   persistido foi Connected", que só muda quando uma nova tentativa começa (a sessão antiga já
   foi derrubada para tentar outra identidade, seja qual for o resultado dela) ou quando um
   `Ambiguous` subsequente é persistido.

   Testes novos pelo `MqttSessionController` (não só pelo coordenador direto): fault injection em
   `reportConnected()` seguida de retry bem-sucedido, confirmando nenhuma chamada extra a
   `restart()`/`poll()` durante o retry e `ready()` falso o tempo todo até persistir; o mesmo para
   `reportAmbiguous()`; e `ready()` permanecendo `true` por 5 ticks ociosos consecutivos depois de
   uma confirmação persistida.

2. **Boot provisionado agora falha fechado de verdade**: com `mqtt_provisioning_enabled=true` e
   perfil completo, `main.cpp` **não chama mais** `session.init(config.network)` nem
   `session.start()` — antes, o código inicializava o store/coordenador de produção mas ainda
   assim conectava a sessão real com as credenciais estáticas de `config.network`, exatamente o
   downgrade silencioso que este perfil existe para impedir. Agora: o store ainda é
   inicializado/validado contra a partição real (prova que a camada durável funciona), um log
   claro declara que o modo provisionado permanece offline até a ligação do controlador, e a
   função retorna sem iniciar nenhuma sessão MQTT — nem com credencial estática, nem com
   nenhuma real (nenhuma existe ainda de qualquer forma). Pixels/renderização local, já
   restaurados antes deste ponto em `app_main()`, continuam ativos (AGENTS.md: renderização
   independe de rede). Modo laboratório (`mqtt_provisioning_enabled=false`) e perfil incompleto
   permanecem exatamente como estavam.

3. **`enterProvisioningFault()` com retorno descartado em `doBootstrap()`**: os casos `401`/`403`
   chamavam `store_.enterProvisioningFault()` sem checar o resultado. Corrigido para capturar o
   retorno e propagá-lo: `commitTransition()` já garante que uma falha nessa escrita nunca muta o
   estado anterior (nada foi mutado antecipadamente), então "preservar o estado anterior" já era
   automático; o que faltava era não descartar o erro. Teste de fault injection dedicado: `401`
   com `DuringCommit` injetado deixa o estado em `Empty` (o anterior, intocado); retry sem falha
   faz a transição para `ProvisioningFault` pousar de verdade.

4. **Documentação**: a ordenação Wi-Fi-antes-de-identidade-MQTT saiu da lista de bloqueios da
   plataforma e passou para "próxima tarefa de arquitetura do firmware" (é uma decisão interna de
   `NetworkSession`, não uma dependência externa); a lista de bloqueios da plataforma ficou
   restrita a `trust_ref`/CA reais, URL de bootstrap, entrega do token, rate limit e merge do
   contrato; e esta seção declara explicitamente que o modo provisionado não usa nenhuma
   credencial estática enquanto a ligação real não existir (item 2 acima).

5. **Escalonamento para `BootstrapRecovery` em uma única transação**: a última tentativa ambígua
   de uma rodada fazia primeiro um commit em `AmbiguousActivation` e, ao atingir o limite, um
   segundo commit em `BootstrapRecovery`. Se apenas o segundo falhasse, repetir o mesmo resultado
   terminal contabilizaria a rodada novamente. O store agora recebe a decisão de recuperação e
   grava estado final e contador incrementado juntos no mesmo slot A/B. O teste injeta falha nessa
   transição no limite: estado e contador permanecem exatamente em `AttemptInFlight`/2; um único
   retry produz `BootstrapRecovery`/3, sem dupla contagem.

Validação desta rodada: build do produto limpo; `validation/` congelada **468/468 ALL_PASS**, sem
nenhum caso novo adicionado nela; `validation_mqtt_credentials/` com os checks novos, **ALL_PASS**
e `run_qemu.py` retornando exit code 0; banner de `51817bf` exato, sem `-dirty`; `git diff
--check` limpo. A correção atômica do item 5 é um commit adicional sobre `b27f9fb`.

## Fechamento e publicação

Nenhuma URL, bearer token, credencial MQTT ou material de CA real foi adicionado em nenhum commit
desta rodada nem na anterior. Todo material de teste novo (fault injection, `hardware_id`
sintético, `MqttSessionController`/`FakeTransportSession`) usa os mesmos valores sintéticos já
estabelecidos (`kHardwareId = "a1b2c3d4e5f6"`, `kTrustRef`/`kCaPem` sintéticos, senhas prefixadas
`SYNTHETIC-`/`ab-`/`durable-fail-`/`rotated-secret-`). `git diff --check` limpo. Durante a
implementação não houve push, merge, PR, flash, acesso a broker real ou a `easyled-platform`.
Após a revisão independente e os gates finais no commit `51817bf`, a branch foi integrada por
fast-forward e publicada em `origin/main` em 15/10/2026. Nenhum deploy, flash ou serviço real foi
acionado nesse fechamento.
