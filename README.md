# PS2DVDPlayer

Player de **DVD-Video em imagens ISO** para PlayStation 2 (homebrew), com
interface própria em português. Os filmes podem vir da **rede (SMB)**, de um
**pendrive USB** ou do **HDD interno**.

Escrito em C com PS2SDK e gsKit; navegação de DVD pela libdvdnav, vídeo
MPEG-2 pelo IPU do PS2 e áudio AC3 pela liba52.

> **Estado:** em desenvolvimento. Tudo abaixo foi testado no emulador
> **PCSX2**; o teste no PS2 físico ainda não foi feito.

## O que funciona

- **Menus do DVD** (First Play, menus com botões e destaque do disco),
  capítulos e troca de clipe/título contínua.
- **Vídeo MPEG-2** com conversão de cor no GS (~30 fps em 480p/NTSC; em
  PAL 576i, a 50 Hz, ~27 fps com descarte controlado de quadros).
- **Áudio AC3** 2.0 e 5.1 (mixado para estéreo), sincronizado com o vídeo,
  com **troca de faixa imediata**.
- **Três fontes de filmes:** SMB (rede local), USB e HDD em exFAT/FAT32,
  inclusive ISOs maiores que 4 GB.
- **Capas dos filmes** (PNG) na tela de detalhes.
- **Configuração de rede/SMB** salva no Memory Card, com teclado virtual.

## Limitações conhecidas

- **Só áudio AC3.** Faixas DTS, LPCM ou MPEG não tocam (não aparecem na troca
  de faixa).
- **Legendas ainda não são exibidas** (o L2 mostra quantas existem).
- **Sem suporte a CSS:** a ISO precisa estar **sem proteção** (como as geradas
  a partir de DVDs próprios com ferramentas de backup).
- **HDD só em exFAT/FAT32 (MBR).** HDD em APA/PFS (formato do HDLoader/OPL
  antigo) não é suportado.
- O **player de DVD nativo do PS2** (o do console) não é usado; fica para um
  projeto separado, a testar no PS2 físico.

## Organização das mídias

Em cada mídia, duas pastas na raiz (separadas das `/DVD` e `/CD` que o OPL usa
para jogos):

```text
FILMES/                     as ISOs de DVD-Video
    Meu Filme.iso
ART/                        as capas (opcional)
    Meu Filme.png
```

| Mídia | Requisito | Onde ficam as ISOs |
|---|---|---|
| USB | pendrive em **exFAT ou FAT32** (MBR) | `usb0:/FILMES` |
| HDD | HDD em **exFAT ou FAT32** (MBR) | `ata0:/FILMES` |
| SMB | compartilhamento **SMB1** | `/<pasta configurada>` (padrão `FILMES`) |

### Capas

- Um **PNG com o mesmo nome da ISO** (`Meu Filme.iso` → `Meu Filme.png`), na
  pasta **`ART`** ao lado da pasta dos filmes. No SMB, `ART` fica ao lado da
  pasta configurada.
- Tamanho da tela: **160 × 224 pixels** (proporção da capa de DVD). Imagens
  maiores (até 1024 × 1536) são reduzidas pelo próprio PS2.
- Sem capa, aparece um disco no lugar.

## Rede e SMB

Na tela **SMB**:

- **Modo:** DHCP ou estático (IP, máscara, gateway e DNS só aparecem no
  estático).
- **Servidor SMB** (IP), **Compartilhamento**, **Pasta** dos filmes,
  **Usuário** e **Senha** (pelo teclado virtual; a senha aparece como `••••`).
- **Aplicar** conecta, lista a pasta e abre a biblioteca.

As preferências ficam em `mc0:/PS2DVDPLAYER/network.cfg` (ou `mc1:`), em texto.
**A senha é gravada em texto puro** no Memory Card.

O cliente SMB do PS2SDK usa **SMB1**: no servidor, o protocolo SMB1/NT1 precisa
estar habilitado (no Samba, por exemplo, `server min protocol = NT1`).

## Controles

**Telas (início, listas, detalhes):** direcional escolhe, ✕ confirma, ○ volta.

**Teclado virtual:** ✕ digita, □ apaga, R1 espaço, △ troca ABC/abc/símbolos,
START confirma, ○ cancela.

**Durante o filme:**

| Botão | Ação |
|---|---|
| START | pausar / continuar |
| L1 / R1 | capítulo anterior / próximo |
| R2 | próxima faixa de áudio |
| L2 | informa as legendas do título |
| △ | menu do disco |
| ○ | parar |
| SELECT | barra de controle (◀▶ escolhe, ✕ confirma) |
| direcional + ✕ | botões dos menus do DVD |

## Modo de vídeo

O PS2 não sabe qual cabo está ligado; o programa usa a configuração do console
(**Configuração do Sistema → Saída de vídeo componente**):

| Configuração do console | Modo usado |
|---|---|
| **Y Cb/Cr** (cabo componente) | **480p** |
| **RGB** ou padrão | **NTSC 480i** ou **PAL 576i**, pela região do console (funciona com qualquer cabo) |

Se a TV não mostrar imagem em 480p, abra o programa **segurando R1**: ele usa o
modo entrelaçado da região.

## Compilar

Requisitos: Docker e Docker Compose (a imagem usa o toolchain do
[ps2dev](https://github.com/ps2dev/ps2dev)).

```bash
docker compose build
docker compose run --rm ps2dvdplayer make
```

O ELF fica em `build/PS2DVDPlayer.elf`. Para recompilar do zero:
`docker compose run --rm ps2dvdplayer make clean all`.

Os recursos gráficos já gerados estão em `assets/`. Para recriá-los (fontes,
logo e botões):

```bash
docker compose run --rm ps2dvdplayer sh tools/make_assets.sh
```

(os botões são recortados do arquivo `Kit de Botões de Controle PlayStation.png`,
que não faz parte do repositório; sem ele, use o `assets/buttons.png` já gerado).

## Testar no PCSX2

- Abrir o ELF: `pcsx2-qt -elf build/PS2DVDPlayer.elf`.
- **Rede:** DEV9 ligado, API *Sockets*.
- **USB:** porta USB do tipo *Mass Storage* com uma imagem MBR + exFAT.
- **HDD:** HDD ligado com uma imagem `.raw` MBR + exFAT (não APA).

Um log de depuração gravado no próprio compartilhamento SMB
(`smb0:/LOGS/ps2dvdplayer.log`, para o PS2 físico) existe no código, mas vem
**desligado** (`DBGLOG_ENABLED` em `include/dbglog.h`).

## Estrutura do código

| Arquivo | Função |
|---|---|
| `src/main.c` | telas, controles e fluxo do programa |
| `src/ui.c`, `src/osk.c` | base gráfica (fontes, botões, ícones) e teclado virtual |
| `src/frame.c`, `src/nav.c` | leitura da ISO, demux, vídeo e navegação (libdvdnav) |
| `src/audio.c` | AC3 (liba52), troca de faixa e sincronia com o vídeo |
| `src/iso.c` | identificação do DVD-Video (títulos, duração, faixas) |
| `src/smb.c`, `src/network.c` | rede e cliente SMB |
| `src/bdm.c` | USB e HDD em exFAT/FAT32 (BDM do PS2SDK) |
| `src/netcfg.c`, `src/memcard.c` | preferências no Memory Card |
| `src/cover.c` | capas (PNG) |
| `tools/make_assets.py` | gera fontes, logo e botões de `assets/` |

## Apoie o projeto

Se o PS2DVDPlayer foi útil para você, considere apoiar o desenvolvimento com uma doação via **Pix** (qualquer valor):

<img src="docs/pix-qrcode.png" alt="QR Code Pix para doação" width="220">

**Pix copia e cola:**

```
00020126580014BR.GOV.BCB.PIX01362c7f2bbb-2b6b-4b41-8b5c-b47a222531d65204000053039865802BR5915VERIDIANO SILVA6008REDENCAO62070503***6304DD08
```

Chave aleatória: `2c7f2bbb-2b6b-4b41-8b5c-b47a222531d6` (Veridiano Silva)

## Licença

**GPL-2.0-or-later** (arquivo [`LICENSE`](LICENSE)).

Componentes de terceiros incluídos:

| Componente | Licença |
|---|---|
| libdvdnav, libdvdread (VideoLAN) — `third_party/` | GPL-2.0-or-later |
| liba52 — `third_party/liba52` | GPL-2.0-or-later |
| Fontes Noto Sans — `third_party/fonts` | SIL Open Font License 1.1 (`OFL.txt`) |
| PS2SDK, gsKit (ligados no build) | licenças próprias de cada projeto |

A base gráfica e o teclado virtual vêm do PS2FileManager (mesmo autor).

Este projeto **não inclui** BIOS, o DVD Player do PS2 nem filmes. Use apenas
cópias de DVDs que você possui.
