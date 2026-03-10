$src = (Join-Path $PSScriptRoot "pintos") -replace '\\', '/'
docker rm -f pintos 2>$null
docker run -it --rm --name pintos --mount "type=bind,source=$src,target=/home/PKUOS/pintos" pkuflyingpig/pintos bash
