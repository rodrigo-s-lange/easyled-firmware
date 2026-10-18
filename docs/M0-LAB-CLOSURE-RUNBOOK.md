# M0 — roteiro de fechamento em bancada

Data de preparação: 2026-10-18  
Firmware de referência: `easyled-firmware/main` em `380d210`  
Unidade M0: `hardware_id=142b2fdcfa5c`, porta local `COM6`

Este arquivo é o checklist operacional para fechar o M0 com uma unidade real.
Ele não contém e nunca deve receber senhas, tokens, segredos ou certificados
privados. Marque um gate somente quando sua evidência estiver registrada.

```mermaid
flowchart LR
    G0["✅ G0<br/>Bancada identificada"] --> G1["⏳ G1<br/>Pacote do backend"]
    G1 --> G2["G2<br/>Perfil local"]
    G2 --> G3["G3<br/>Identidade na NVS"]
    G3 --> G4["G4<br/>Boot provisionado"]
    G4 --> G5["G5<br/>MQTT + telemetria"]
    G5 --> G6["G6<br/>Comando real"]
    G6 --> G7["G7<br/>Falhas e reboot"]
    G7 --> G8["G8<br/>M0 encerrado"]
```

## Estado resumido

| Gate | Estado | Responsável principal | Resultado exigido |
|---|---|---|---|
| G0 — bancada | ✅ concluído | Firmware/lab | Placa, porta e identidade física confirmadas |
| G1 — pacote do backend | ⏳ aguardando | Plataforma | Ambiente e identidade cadastrada, sem lacunas de TLS |
| G2 — perfil local | ⬜ pendente | Firmware/lab | Build provisionado sem segredo no Git |
| G3 — identidade NVS | ⬜ pendente | Firmware/lab | Segredo real persistido e recuperado após reboot |
| G4 — bootstrap | ⬜ pendente | Conjunto | HTTPS 200, credencial MQTT durável, WSS conectado |
| G5 — telemetria | ⬜ pendente | Conjunto | `state` e `capability_reported` ingeridos e correlacionados |
| G6 — comando real | ⬜ pendente | Conjunto | `apply_config` chega e termina em `command_applied` |
| G7 — recuperação | ⬜ pendente | Conjunto | Reboot, duplicata e falha preservam o estado correto |
| G8 — aceite | ⬜ pendente | Rodrigo | Pacote de evidências revisado e M0 declarado concluído |

## G0 — bancada identificada ✅

- [x] Repositório correto: `easyled-firmware`.
- [x] `main` e `origin/main`: `380d210`.
- [x] Porta: `COM6`, CP210x operacional.
- [x] Chip: ESP32-D0WD-V3, revisão 3.1.
- [x] MAC eFuse: `14:2b:2f:dc:fa:5c`.
- [x] `hardware_id`: `142b2fdcfa5c`.
- [x] Saída ativa: GPIO16.
- [x] 372 pixels GRB, canvas 31×12.
- [x] Serpentina por colunas: coluna 0 sobe, coluna 1 desce.
- [x] Origem: pixel 0 no canto inferior esquerdo; pixel 371 no canto superior direito.
- [x] GPIO17 fisicamente desconectada e ausente do payload M0.
- [x] Build do produto aprovado; validações: bootstrap 164, contrato 468,
  credenciais MQTT 298 e sessão MQTT 98, todas `ALL_PASS`.

## G1 — pacote obrigatório do backend ⏳

Não iniciar apagamento ou provisionamento da NVS até este pacote estar
completo. A plataforma deve entregar por canal apropriado:

### Bloqueio de sequência identificado em 2026-10-18

A sequência proposta pela plataforma contém duas operações que o firmware
atual ainda não implementa:

- abrir `claimability` e executar o claim pela rede;
- obter e persistir o bearer HTTPS por
  `POST /api/v1/device-bootstrap/{hardware_id}/https-credential`.

Além disso, o boot provisionado falha fechado antes de iniciar a rede quando
`device_id`, `hardware_binding_id` ou `generation` estão ausentes, e a resposta
da emissão MQTT é aceita somente quando repete exatamente essa mesma tupla.
Portanto, a placa não consegue criar pela rede os identificadores que ela já
precisa possuir para começar o bootstrap.

Para fechar o M0 de uma unidade sem ampliar o produto durante a janela de
bancada, a coreografia proposta é:

1. `m0-provision-board` cadastra a unidade, gera e guarda os dois segredos e
   deixa a unidade claimable sem exigir que a placa esteja online;
2. o proprietário conclui o claim;
3. a plataforma entrega por canal protegido a tupla não secreta
   `device_id`/`hardware_binding_id`/`generation`, o segredo bootstrap e,
   somente para este ensaio M0, o bearer HTTPS;
4. o laboratório grava o segredo bootstrap na NVS e monta o perfil local
   ignorado pelo Git com a tupla, bearer, origens e material TLS;
5. a placa inicia, obtém a credencial MQTT pelo endpoint real e continua o
   fluxo de telemetria, `apply_config` e ACK terminal.

Se os passos 1 e 2 não forem possíveis sem a placa online, G1 permanece
bloqueado e será necessário implementar primeiro os clientes de claimability,
claim e credencial HTTPS. Essa é uma frente de produto maior e não deve ser
improvisada durante o ensaio.

- [ ] Confirmação de que `hardware_id=142b2fdcfa5c` foi cadastrado na linha de
  fabricação/binding correto.
- [ ] `device_id`, `hardware_binding_id` e `generation` definitivos para esta
  unidade.
- [ ] Um segredo de bootstrap operacional exclusivo, 32 bytes aleatórios,
  entregue fora do Git e fora deste documento.
- [ ] Confirmação de que o backend armazenou SHA-256 dos **43 caracteres ASCII
  base64url sem padding**, e não dos 32 bytes binários.
- [ ] Origem HTTPS implantada para o bootstrap, sem o sufixo da rota. O firmware
  acrescenta `/api/v1/device-bootstrap/{hardware_id}/mqtt-credential`.
- [ ] Confirmação de que essa rota está implantada e responde para esta unidade.
- [ ] `trust_ref` definitivo devolvido pela rota.
- [ ] PEM da CA associado ao `trust_ref`. **O resolver atual do firmware exige
  PEM não nulo inclusive quando o certificado do WSS tem raiz pública.** Se a
  plataforma quiser selecionar diretamente o bundle público do ESP-IDF, será
  necessária uma pequena mudança de contrato/firmware antes do ensaio.
- [ ] URI `wss://` e política/tópicos que serão emitidos na credencial MQTT.
- [ ] Credencial HTTPS do dispositivo para baixar configurações, entregue fora
  do Git, ou uma decisão explícita de que o primeiro M0 não exercerá download
  remoto de `apply_config`.
- [ ] Backend implantado com ingestão de `state`, `capability_reported` e ACKs,
  além de persistência/publicação de `apply_config` e reconciliação terminal.

### Mensagem curta para a equipe de backend

> A resposta foi recebida, mas há uma divergência de ordem que impede o ensaio
> literal. O firmware atual não possui cliente de claimability/claim nem do
> endpoint `/https-credential`. Ele também exige `device_id`,
> `hardware_binding_id` e `generation` antes de iniciar a rede e usa essa tupla
> para validar a credencial MQTT. Confirmem, por favor: (1) o
> `m0-provision-board` consegue cadastrar e deixar a unidade claimable sem a
> placa online; (2) o proprietário consegue concluir o claim antes do primeiro
> boot provisionado; (3) depois do claim vocês entregarão a tupla definitiva;
> e (4) para este M0 o bearer HTTPS pode ser entregue por artefato protegido e
> inserido no perfil local ignorado pelo Git. Se qualquer resposta for não,
> precisamos pausar a bancada e implementar primeiro os fluxos de claim e
> credencial HTTPS. Nenhum segredo deve ser enviado por chat. Continuamos
> aguardando também a origem implantada, URI WSS, `trust_ref`, PEM e SHA-256,
> além da confirmação operacional dos pipelines reais.

## G2 — montar o perfil provisionado local

Somente depois de G1:

- [ ] Criar `main/network_credentials.hpp` local, que já é ignorado pelo Git.
- [ ] Preencher Wi-Fi da bancada.
- [ ] Preencher `device_id`, `hardware_binding_id`, `generation` e versão do
  firmware.
- [ ] Preencher origem do bootstrap, `trust_ref` e PEM da CA.
- [ ] Preencher token HTTPS de configuração e CA correspondente quando usado.
- [ ] Definir `mqtt_provisioning_enabled=true`.
- [ ] Confirmar que nenhum segredo aparece em `git status`, diff, log de build
  ou saída serial.
- [ ] Compilar o produto e registrar tamanho/heap estático.

**Gate:** perfil completo compila; perfil incompleto falha fechado e não usa
as credenciais MQTT de laboratório.

## G3 — backup e provisionamento físico da identidade

O `easyled` é uma partição NVS compartilhada. O gerador de identidade produz
uma imagem completa dessa partição e só aceita uma partição totalmente vazia.

1. [x] Fazer backup integral da flash e, separadamente, dos 128 KiB de
   `easyled` em `0x190000`.
2. [x] Guardar hashes SHA-256 dos backups sem publicar seu conteúdo.
3. [x] Inspecionar o dump. Em 2026-10-18 ele continha exatamente 131072 bytes
   `0xFF`, sem namespace ou valor gravado: a partição já está apagada.
4. [x] Nenhum apagamento foi necessário. Se uma nova leitura mostrar dados
   antes do provisionamento, parar e reavaliar; nunca usar `erase-flash`.
5. [ ] Gerar a imagem com
   `tools/provision_bootstrap_identity.py`, usando entrada oculta ou variável
   temporária. Nunca passar o segredo na linha de comando.
6. [ ] Gravar exatamente 128 KiB em `0x190000`.
7. [ ] Ler a partição de volta e comparar o hash da imagem gravada.
8. [ ] Reiniciar e confirmar por serial apenas `hardware_id` e estado `Valid`;
   o segredo nunca deve aparecer.

**Gate:** percurso segredo textual → NVS binária → reboot → autorização
base64url funciona sem expor o segredo.

Evidência local preservada fora do repositório:

| Arquivo | SHA-256 |
|---|---|
| `flash-full-before.bin` | `4e3eada343ee19c14179c6663dfcca4ddf9311fa803d5b9a413aac415da3d2e4` |
| `easyled-before.bin` | `b5a41c3758763bbec72769fab4a2533bf2db0b6312d93d25a695f9e4b9e02260` |

Diretório local: `C:\Users\Familia\.codex\artifacts\easyled-m0-lab-20261018`.

## G4 — primeiro boot provisionado e bootstrap HTTPS

- [ ] Gravar bootloader/tabela/app sem apagar a partição `easyled`.
- [ ] Capturar boot antes de o relógio estar confiável: Wi-Fi pode subir, mas
  TLS/MQTT não deve iniciar antes de `2024-01-01` no relógio do sistema.
- [ ] Confirmar sincronização NTP.
- [ ] Confirmar POST HTTPS para a rota de bootstrap com validação de hostname e
  certificado ativa.
- [ ] Confirmar HTTP 200 e validação de `device_id`, binding e geração.
- [ ] Confirmar credencial recebida primeiro como `PendingUnused` e persistida
  antes do primeiro uso.
- [ ] Confirmar conexão WSS, CONNACK e SUBACK.
- [ ] Confirmar promoção durável para `CurrentConfirmed`.
- [ ] Reiniciar a placa e provar reconexão com a credencial durável.

**Gate:** nenhuma credencial estática de laboratório foi usada e não houve
fallback silencioso.

## G5 — telemetria observável no backend

- [ ] `state` recebido com `device_id`, `hardware_binding_id`, `generation`,
  `boot_counter`, `state_sequence`, versão e configuração ativa.
- [ ] `capability_reported` recebido com o mesmo binding/geração, catálogo e
  `max_payload_bytes=8192`.
- [ ] PUBACK observado no firmware.
- [ ] Persistência/ingestão confirmada separadamente no backend; PUBACK sozinho
  não encerra este gate.
- [ ] Desconectar/reconectar uma vez e confirmar reenvio consistente.

## G6 — primeiro comando real de configuração

Usar um payload M0 específico da matriz física, com 372 pixels e ledmap por
**colunas**. Os vetores P1 `bench-31x12-config-v1/v2` usam outra orientação e
não devem ser enviados à matriz real como se descrevessem sua fiação.

- [ ] Criar/persistir `apply_config` no backend.
- [ ] Publicar automaticamente no tópico desta unidade, QoS 1 e sem retain.
- [ ] Observar `command_received`.
- [ ] Observar `command_accepted`.
- [ ] Baixar os bytes canônicos por HTTPS e conferir SHA-256 antes de parsear.
- [ ] Aplicar uma cena visual inequívoca na matriz 31×12.
- [ ] Persistir e ativar no frame boundary.
- [ ] Observar `command_applied` somente após persistência e ativação.
- [ ] Confirmar a mesma configuração no `state` reconciliado pelo backend.

## G7 — recuperação e comportamento terminal

- [ ] Reboot: configuração, credencial MQTT e identidade continuam válidas.
- [ ] Duplicata do mesmo `command_id`: republica o resultado terminal sem
  baixar, gravar ou reaplicar.
- [ ] Checksum incorreto: `command_rejected`, cena anterior preservada.
- [ ] Falha simulada de download: cena anterior preservada.
- [ ] Perda de conexão antes do PUBACK: reenvio sem criar novo resultado ou
  novo `state_sequence` para o mesmo relatório pendente.
- [ ] Dez minutos ocioso através do caminho WSS/Cloudflare, com keepalive e
  reconexão registrados.
- [ ] Registrar heap livre antes do TLS, após HTTPS, após WSS e durante apply.
- [ ] Verificar visualmente ausência de flicker durante persistência em flash.

## G8 — critério de encerramento

O M0 está fechado quando existe um pacote único contendo:

- [ ] SHAs exatos do firmware e da plataforma implantada.
- [ ] Identidade da unidade e perfil físico, sem segredos.
- [ ] Logs redigidos do boot, bootstrap, WSS e reboot.
- [ ] Evidência backend de `state`, capacidade e ciclo completo do comando.
- [ ] Evidência visual do payload aplicado e recuperado após reboot.
- [ ] Resultado dos casos de duplicata, checksum inválido e reconexão.
- [ ] Medidas de heap, tempo e estabilidade do ensaio.
- [ ] Lista explícita do que continua fora do M0: rebind/revogação completa,
  OTA de produto, segurança irreversível de fabricação e experiência final do
  cliente.

## Regra operacional

Executar um gate por vez. Se um gate falhar, preservar a configuração ativa,
registrar a evidência e corrigir a causa antes de avançar. Não compensar uma
falha do backend com fixture embarcada nem trocar silenciosamente para o modo
laboratório.
