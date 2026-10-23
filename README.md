# PS2DVDPlayer

Primeiro prototipo de um player de DVD-Video para PlayStation 2.

## Objetivo desta versao

Esta primeira etapa valida o ambiente de compilacao e gera um ELF minimo para o PS2.

## Requisitos

- Linux
- Docker
- Docker Compose

## Compilar

Na raiz do projeto:

```bash
docker compose build
docker compose run --rm ps2dvdplayer
```

O ELF sera gerado em:

```text
build/PS2DVDPlayer.elf
```

Tambem e possivel usar:

```bash
docker compose run --rm ps2dvdplayer make clean
```

## Estrutura

```text
PS2DVDPlayer/
├── Dockerfile
├── docker-compose.yml
├── Makefile
├── README.md
├── include/
├── src/
│   └── main.c
└── build/
```

## Proximas etapas

1. Inicializacao de video.
2. Controle DualShock 2.
3. Inicializacao USB.
4. Navegacao em `mass:/FILMES/` (separado de `/DVD`, usado pelo OPL).
5. Lista de arquivos `.iso`.
6. Leitura da ISO e deteccao de `VIDEO_TS`.
7. Player DVD-Video.
8. HDD e SMB.
