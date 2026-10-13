# P1 — Validação dos vetores de compatibilidade do config-wire (2026-10-13)

## Resumo executivo

1. **Baseline**: branch `main`, commit `4e56c37378bf166c8a7a547dcf378cd69bfcf850` — exatamente o
   esperado, sem divergência. QEMU baseline: **405/405 ALL_PASS**, sem panic/watchdog. **O parser
   produtivo validado nesta rodada é este commit, `4e56c37`** — nenhum componente em `components/`
   ou `main/` foi alterado em nenhum momento.
2. **Resultado final**: todos os 9 vetores P1 comportam-se exatamente como documentado contra o
   firmware atual. **Nenhuma mudança em código produtivo** foi necessária. **O commit que contém a
   suíte/evidência (fixtures + testes + esta documentação) é `a3664bc31e46c1f38fa8798dfe1a6a11245a3b4b`**,
   confirmado como `App version: a3664bc` (sem `-dirty`) no banner de boot do QEMU após uma
   recompilação limpa a partir dele.
3. **Contagem de checks**: 405 → **468/468 ALL_PASS** (+63), zero falhas, zero panic/watchdog.
4. **Arquivos adicionados/modificados**: 9 fixtures novas em `validation/fixtures/p1-config-download/`,
   `validation/main/CMakeLists.txt` (wiring `EMBED_TXTFILES`), `validation/main/main.cpp` (testes),
   este documento. Nenhum arquivo em `components/` ou `main/` foi tocado.
5. **Tarefas**: todas concluídas. Nenhuma correção de código produtivo foi necessária (todos os
   vetores já passavam contra o firmware atual).
6. **Impedimentos concretos**: um, de memória (não de comportamento) — ver "Memória da validação"
   abaixo; contornado reduzindo redundância de teste, sem perder cobertura de afirmação alguma.

## Procedência imutável

Importado byte a byte (confirmado por tamanho e SHA-256 nos dois lados) de:

- Repositório upstream: `rodrigo-s-lange/easyled-platform`
- Commit congelado: `8cc828c00db339b683b83232e22b354c417ecc81`
- Caminho upstream: `docs/fixtures/config-download/`
- Importado via `git show <SHA>:<path>` em um clone local read-only da plataforma (nenhum checkout,
  reset, merge ou edição na plataforma; `easyled-platform`'s HEAD permaneceu em `ceb30b1` durante
  toda a operação).

| Arquivo | Bytes | SHA-256 | Newline final |
|---|---:|---|---|
| `sample-config-payload.json` | 537 | `112ef822a45000a449e87b97170d5448cdfc2afbc11c33728e39a1095b4eca6f` | não |
| `bench-31x12-config-v1.json` | 1920 | `f41ed2ef4c4a9c9987db76ec83cd20f4242eb8311f563417e2eb2173fd62ea55` | não |
| `bench-31x12-config-v2.json` | 1920 | `3ed200172436067da282bf6dffb93a91507f79b1a8df8547d1256fdc93d29e68` | não |
| `two-outputs-holes-config.json` | 1392 | `f73864d8215641a7750f3120580c78c00fd56d0f0a0a2c99cd3e204b82672116` | não |
| `invalid-ledmap-schema-version-2.json` | 537 | `c0d674d67977e4db7a946c1b5a1d46ddad55e42c30ea0aebbb7099e1482592fa` | não |
| `invalid-ledmap-map-length-mismatch.json` | 534 | `acb3c45eb7bf69457ad912dcc59866edf0d2575a057dad827faf30cec5dbf572` | não |
| `invalid-ledmap-duplicate-index.json` | 537 | `1ffb0e92bf74e36654b858dde0ccda201ed4f631b13b3a53417c2acf5c8590d6` | não |
| `invalid-ledmap-index-out-of-range.json` | 537 | `d4f9b881a2b742bd549092c492a14f55cfca4e8ba8ef9e9b95275a2170e2714f` | não |
| `invalid-segment-color-short-hex.json` | 555 | `3d396e9c0a5c7f0a8b81979523ff77e2f294ea00ecc2945ba207a947729a0fb9` | não |

Todos os 9 têm verificação automatizada permanente de tamanho+SHA-256 (`parseSha256`+`verifySha256`
contra os bytes reais incorporados via `EMBED_TXTFILES`) — falha se qualquer byte mudar.
`sample-config-payload.json` é byte a byte idêntico (mesmo SHA-256) ao fixture canônico 4×4 da PR #72
já testado em detalhe pela suíte existente; não foi re-verificado campo a campo aqui além da
procedência, por ser redundante.

## Resultados por vetor

| Vetor | Resultado esperado | Resultado obtido | Onde é provado |
|---|---|---|---|
| `sample-config-payload.json` | aceito | aceito (idêntico ao fixture PR #72 já coberto) | procedência + suíte existente |
| `bench-31x12-config-v1.json` | aceito | aceito; outputs/ledmap/segmento/cor conferidos | parser direto + staging/reboot real |
| `bench-31x12-config-v2.json` | aceito; substitui v1 duravelmente | aceito; substitui v1, sobrevive a 2 reboots simulados | parser direto + staging/reboot real |
| `two-outputs-holes-config.json` | aceito; 2 saídas, buracos, fronteira entre saídas | aceito; buracos e índices 90/99/100/104 conferidos | parser direto |
| `invalid-ledmap-schema-version-2.json` | `SchemaError::OutOfRange` | `OutOfRange`; `payload_out_of_range` no wire | parser direto + Executor real |
| `invalid-ledmap-map-length-mismatch.json` | `SchemaError::OutOfRange` | `OutOfRange` | parser direto (staging/v2 inalterados) |
| `invalid-ledmap-duplicate-index.json` | `SchemaError::OutOfRange` | `OutOfRange` | parser direto (staging/v2 inalterados) |
| `invalid-ledmap-index-out-of-range.json` | `SchemaError::OutOfRange` | `OutOfRange` | parser direto (staging/v2 inalterados) |
| `invalid-segment-color-short-hex.json` | `SchemaError::Malformed` | `Malformed`; `malformed_payload` no wire | parser direto + Executor real |

Confirmado também, direto do código (`pixels_payload_adapter.cpp`): `#RRGGBB` aceita hex em
qualquer caixa (`isHex()` aceita `a-f` e `A-F` igualmente) — provado por um vetor **positivo** real
(`two-outputs-holes-config.json` usa `"#00FF80"` e `"#ff0000"`, ambos parseados para os mesmos
valores numéricos); `#RGB` (3 dígitos) continua rejeitado como `Malformed`
(`invalid-segment-color-short-hex.json`, `"#fff"`).

## Sequência real v1 → v2 (staging/reboot reais)

Executada dentro de `runDurableStorageChecks()`, reaproveitando a MESMA `NvsDurableStaging`/
`NvsTerminalJournal`/`ActivationTarget` real que o executor usa (sem camada Executor/PayloadSource
aqui — o fingerprint exato de bytes do `RecordingActivationTarget` já prova persistência/ativação;
a correção de schema é provada separadamente contra o parser real, acima):

1. Parte de uma configuração ativa conhecida (o preset já confirmado ativo nos testes anteriores
   da mesma função).
2. Aplica v1 (`staging.begin/append/verifyAndCommit/activateAtFrameBoundary`) — confirmado
   persistido e ativo.
3. Simula reboot (`staging.end()` + `staging.init(target, journal)`) — v1 confirmado ainda ativo.
4. Aplica v2 — confirmado que substitui v1 (fingerprint de bytes diferente, v1 não mais ativo).
5. Simula outro reboot — v2 confirmado ainda ativo.
6. Para cada um dos 5 vetores negativos: `parseConfigPayload()` direto confirma a categoria exata
   **e** confirma que `target`/`staging` continuam mostrando v2, inalterado — prova de que nenhuma
   gravação durável e nenhuma ativação acontecem como efeito colateral de uma tentativa rejeitada
   (consistente com o próprio código do executor: o schema é checado **antes** de `staging.begin()`
   ser chamado, nunca depois).

`ledmap.version` permanece exatamente `1` em v1 **e** v2 — a segunda configuração é uma nova
**revisão de autoria** (tabela `map` diferente, mesmo esquema), nunca um bump de esquema. Ver
"Distinção de versões" abaixo.

## Distinção: versão de esquema, revisão de autoria, versão de config

Três conceitos diferentes que este pacote P1 deixa claros pela primeira vez com dados reais:

- **`ledmap.version`** (esquema, no wire): fixo em `1` para todo este MVP. `unsignedValue()`'s
  próprio limite para este campo é o intervalo exato `[1,1]` — não um piso "≥1" — confirmado
  rejeitando `invalid-ledmap-schema-version-2.json` (`version:2`) como `OutOfRange`.
- **Revisão de autoria do mapa** (conteúdo, não wire): v1 e v2 têm o MESMO `ledmap.version=1`, mas
  tabelas `map` diferentes — é apenas uma nova revisão de quem autorou o ledmap, sem relação com o
  campo de esquema.
- **`config_version`** (campo do comando, não do payload): não testado numericamente aqui além do
  valor fixo usado no envelope sintético; é um conceito totalmente separado do `ledmap.version` do
  payload — nenhuma correlação entre os dois deve ser assumida.

## Topologia: esclarecido — vetores de compatibilidade, não a bancada física

**`bench-31x12-config-v1.json` e `bench-31x12-config-v2.json` são vetores de compatibilidade do
wire** (nome e dimensões 31×12/372 escolhidos para exercitar o mesmo tamanho da bancada, mas o
conteúdo do `map` não é a fiação física real). O documento `docs/M0-FIRMWARE-NOITE.md` (rodada
anterior) registrou um vetor **hipotético** de canto para a bancada física, escrito antes de este
fixture existir, assumindo serpentina **por colunas** de 12 (a fiação real de Rodrigo: GPIO16,
372 pixels, 31 colunas de 12, coluna 0 sobe, coluna 1 desce, ...).

Extraindo os valores reais de `bench-31x12-config-v2.json` (via parser JSON, não de cabeça):

```
índice 0   = 0      índice 11  = 11     índice 12  = 12     índice 23  = 23
índice 31  = 61      índice 61  = 31
índice 359 = 353     índice 360 = 352    índice 371 = 341
```

A linha 0 (índices 0-30) é identidade pura; a partir da linha 1 (índice 31), cada linha de 31
inverte de sentido alternadamente — isto é **serpentina por linhas de 31**, não por colunas de 12.
**Esclarecido**: isso não é um erro meu de convenção de eixo nem um erro do fixture — os dois
vetores deliberadamente usam uma serpentina diferente da bancada física, porque seu único
propósito é exercitar a mecânica do wire (bijeção real, não identidade; `ledmap.version=1` como
revisão de autoria; substituição durável v1→v2) e não representar nem ser aplicado na bancada real
de Rodrigo. **Nenhuma aplicação física de `bench-31x12-config-v1.json` ou
`bench-31x12-config-v2.json` foi realizada nesta rodada nem está prevista** — a sequência v1→v2
descrita acima é inteiramente QEMU, contra armazenamento NVS real emulado, nunca contra pixels
físicos.

O vetor de canto físico do M0 (coluna-serpentina de 12) permanece correto para a bancada real;
ele só não deve ser comparado a estes dois fixtures, que nunca pretenderam representá-la. O
procedimento de bancada física (GPIO16, 372 pixels, 31×12, serpentina por colunas) continua
documentado em `docs/M0-FIRMWARE-NOITE.md` — não duplicado aqui.

## Limites

- `capabilities.max_payload_bytes=8192`, `NetworkSession::kMaxMessageBytes=1024`, comportamento
  antes do NTP, e `reboot`/`ota` como `unsupported_command` — todos já documentados e confirmados em
  `docs/M0-FIRMWARE-NOITE.md`; esta rodada não os reexamina.

## Memória da validação

| | Antes (baseline, commit `4e56c37`) | Depois (com os 9 vetores P1) |
|---|---|---|
| Checks QEMU | 405/405 ALL_PASS | 468/468 ALL_PASS |
| Binário de validação | — | 741296 B |
| DRAM estático | 177740 B (98,34%, 2996 B livres) | 179964 B (**99,57%**, **772 B livres**) |
| IRAM estático | 48047 B (36,66%) | 48047 B (36,66%, inalterado) |
| Produto (não tocado) | 1110352 B, DRAM 73439 B (40,63%), IRAM 90555 B (69,09%) | idêntico |

A margem ficou crítica (772 B). Conforme a própria orientação desta tarefa ("divida os vetores em
um runner focado em vez de remover cobertura"), **nenhuma cobertura foi removida** — em vez disso:

- `payload_candidate` (já existente, global) foi reaproveitado para todo o parsing direto dos 9
  vetores, sem nenhuma instância nova de `Candidate`;
- `NvsDurableStaging`/`NvsTerminalJournal`/`RecordingActivationTarget` reais e já existentes (de
  `runDurableStorageChecks()`) foram reaproveitados para a sequência v1→v2→reboot — uma segunda
  instância de `NvsDurableStaging` sozinha teria ~9,4 KB (o buffer `record_[kWireBytes]` de
  8326 B + scratch), inviável;
  `reboot_executor`/`payload_source`/`event_sink6` (já existentes, do bloco reboot/ota) foram
  reaproveitados para a prova de reason_code no wire — nenhum `Executor` novo (cada um já custa
  ~9 KB pelo próprio `payload_buffer_[8192]`);
- um único buffer `int32_t[372]` novo foi necessário para `materializeLedmap()` (1488 B,
  reaproveitado sequencialmente entre v1/v2/two-outputs).
- **Único ajuste real por memória**: dos 5 vetores negativos, a prova "reason_code real no wire via
  Executor" roda para 2 representativos (um de cada `reason_code` distinto:
  `payload_out_of_range`/`malformed_payload`) em vez dos 5 — não por dúvida sobre os outros 3, mas
  porque cada chamada adicional ao `Executor` real neste ponto específico da suíte (cJSON parseando
  + serializando sobre um heap dinâmico já minúsculo, dado o `.bss` estático em 99,57%) tem um pico
  de alocação que a partir da 3ª chamada consecutiva falha com `ESP_ERR_NO_MEM` -- **confirmado não
  ser um leak**: auditei `parseConfigPayload()` e `serializeEvent()` linha a linha e todo caminho de
  retorno libera sua árvore cJSON corretamente. É puramente o teto de heap deste ponto da suíte. A
  categoria exata (`SchemaError`) dos outros 3 já está provada, sem este custo, direto contra o
  parser dentro de `runDurableStorageChecks()`; e o mapeamento categoria→reason_code
  (`schemaErrorCode()`) é um `switch` de 4 casos puro, já lido e citado acima — os 2 casos
  exercitados no wire cobrem as duas strings distintas que esse switch pode produzir.

**Este runner (`validation/`, target `easyled_firmware_validation`) fica congelado em termos de
capacidade de crescimento**: com apenas 772 B de DRAM estática livre, não há margem para mais
fixtures/testes relevantes aqui. Qualquer suíte nova que precise de memória estática adicional
(novos fixtures, novos `Candidate`/`Executor`/`NvsDurableStaging`, buffers de materialização
maiores) deve usar outro target de validação, não este.

## QEMU vs. evidência física

Tudo acima é **QEMU**, nenhum hardware real. Nenhuma alegação de bancada física é feita nesta
rodada. O roteiro de testes físicos (incluindo os casos que dependem desta rodada, como "duas
configurações distintas aplicadas em sequência") continua em `docs/M0-FIRMWARE-NOITE.md`, com
status `NÃO EXECUTADO — requer janela coordenada de bancada`.

## Tabela de resposta para a equipe da plataforma

| Vetor | Bytes/SHA conferidos | Resultado esperado | Resultado obtido | Parser/firmware commit | Configuração anterior preservada |
|---|---|---|---|---|---|
| sample-config-payload.json | sim | aceito | aceito | `4e56c37` | n/a (primeira aplicação) |
| bench-31x12-config-v1.json | sim | aceito | aceito | `4e56c37` | sim (preset anterior preservado até a aplicação) |
| bench-31x12-config-v2.json | sim | aceito, substitui v1 | aceito, substitui v1; sobrevive a 2 reboots | `4e56c37` | sim (v1 preservado até a aplicação) |
| two-outputs-holes-config.json | sim | aceito | aceito | `4e56c37` | n/a (testado isoladamente) |
| invalid-ledmap-schema-version-2.json | sim | `OutOfRange` | `OutOfRange` / `payload_out_of_range` | `4e56c37` | sim (v2 preservado) |
| invalid-ledmap-map-length-mismatch.json | sim | `OutOfRange` | `OutOfRange` | `4e56c37` | sim (v2 preservado) |
| invalid-ledmap-duplicate-index.json | sim | `OutOfRange` | `OutOfRange` | `4e56c37` | sim (v2 preservado) |
| invalid-ledmap-index-out-of-range.json | sim | `OutOfRange` | `OutOfRange` | `4e56c37` | sim (v2 preservado) |
| invalid-segment-color-short-hex.json | sim | `Malformed` | `Malformed` / `malformed_payload` | `4e56c37` | sim (v2 preservado) |

## Divergências

1. **Nenhuma divergência de comportamento** entre o código real e os 9 resultados documentados —
   todos bateram exatamente.
2. **Resolvido, não é mais uma divergência aberta**: a aparente incompatibilidade entre o vetor de
   canto hipotético do M0 (coluna-serpentina de 12, bancada física) e os valores reais de
   `bench-31x12-config-v2.json` (linha-serpentina de 31) — ver seção "Topologia" acima. Os dois
   fixtures são vetores de compatibilidade do wire, nunca destinados a representar a fiação física
   de Rodrigo; não há erro de convenção de eixo nem erro no fixture.

## Impedimentos

Um, de memória, já contornado sem perda de cobertura (ver "Memória da validação"). Nenhuma
dependência externa concreta ficou pendente para o trabalho desta rodada especificamente — o
ensaio físico completo continua dependendo da janela de bancada já registrada no M0.
