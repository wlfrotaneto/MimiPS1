# Guia Completo de Recompilação de Jogos de PS1 no Mac

Este guia explica passo a passo como extrair o binário de um jogo de PlayStation 1 a partir de uma imagem `.iso` ou `.bin/.cue` e recompilá-lo em um arquivo `.dmg` nativo para macOS no Apple Silicon (Mac Mini M4).

---

## 1. Como obter o executável principal do jogo

Nos discos de PlayStation 1, o executável principal é referenciado no arquivo de configuração `SYSTEM.CNF` na raiz do disco.

Exemplo de conteúdo do `SYSTEM.CNF`:
```text
BOOT = cdrom:\SLUS_000.67;1
TCB = 4
EVENT = 16
STACK = 801FFFF0
```

Nesse exemplo, o arquivo `SLUS_000.67` (ou `SCES_xxx.xx`, `SLES_xxx.xx`, `PSX.EXE`) é o executável MIPS (`PS-X EXE`).

Você pode extrair esse arquivo montando a imagem `.iso` ou usando ferramentas como `7z` ou `bchunk`:
```bash
# Se tiver o bin/cue:
7z x jogo.iso SLUS_000.67
```

---

## 2. Recompilando o jogo com o MimiPS1

Com o arquivo executável em mãos (por exemplo, `SLUS_000.67`):

### Método Automático (1 comando):
```bash
python3 tools/mimips1_pipeline.py pipeline SLUS_000.67 --name "MeuJogoPS1"
```

O comando fará tudo:
1. Recompilará o código MIPS para C.
2. Compilará o binário nativo ARM64 usando o compilador Apple Clang otimizado para o M4.
3. Criará o pacote `MeuJogoPS1.app`.
4. Gerará o instalador `dist/MeuJogoPS1-AppleSilicon.dmg`.

---

## 3. Instalando e Jogando no Mac

1. Dê um duplo clique no arquivo `.dmg` gerado na pasta `dist/`.
2. Uma janela nativa do macOS se abrirá mostrando o ícone do jogo e um atalho para a pasta **Applications**.
3. Arraste o ícone do jogo para a pasta **Applications**.
4. Conecte seu controle de PS5 (DualSense), PS4, Xbox ou Switch via Bluetooth.
5. Abra o jogo diretamente pelo Launchpad ou Spotlight. O jogo rodará nativamente a 60 FPS com Metal e CoreAudio!
