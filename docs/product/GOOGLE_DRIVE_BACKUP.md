# Backup no Google Drive

O recurso fica em **Ferramentas > Backup no Google Drive**. Ele usa exclusivamente a pasta privada
`appDataFolder` do Google Drive; portanto, não solicita acesso aos demais arquivos do usuário.

## Preparação da integração

1. Crie ou selecione um projeto no Google Cloud Console.
2. Ative a **Google Drive API**.
3. Configure a tela de consentimento OAuth e adicione o escopo não sensível
   `https://www.googleapis.com/auth/drive.appdata`.
4. Crie um Client ID OAuth do tipo **Aplicativo para computador**.
5. Defina `PRODUCT_GOOGLE_DRIVE_CLIENT_ID` ao configurar o build ou informe o Client ID no próprio painel.

Em builds de desenvolvimento, também é possível usar a variável de ambiente
`OBS_GOOGLE_DRIVE_CLIENT_ID`. O fluxo usa navegador externo, callback loopback e PKCE S256.

## Conteúdo e segurança

- Preferências, perfis, coleções de cenas e configurações de plugins são selecionáveis.
- Logs, gravações, caches, cookies/sessões do navegador, dumps e atualizações não entram no pacote.
- Por padrão, chaves, senhas e tokens reconhecidos em JSON/INI são removidos.
- A opção de incluir credenciais protege todo o arquivo com AES-256-GCM e chave derivada por
  PBKDF2-HMAC-SHA256. A senha nunca é armazenada nem pode ser recuperada.
- Backups automáticos sempre removem credenciais.
- Antes de uma restauração, uma cópia local de segurança é criada em `obs-studio/backups/cloud`.

## Restauração entre computadores

A restauração é preparada e aplicada somente no próximo início do OBS, antes do carregamento dos
perfis e das cenas. Caminhos de mídia podem ser substituídos na tela de restauração. Quando fontes
com o mesmo nome já existem, a opção de preservar dispositivos mantém as câmeras, placas de captura
e dispositivos de áudio configurados no computador de destino. Em backups criptografados, as
credenciais podem ser restauradas ou preservadas separadamente das demais categorias.

Arquivos adicionais já existentes não são apagados; apenas os itens presentes nas categorias
selecionadas são substituídos atomicamente.
