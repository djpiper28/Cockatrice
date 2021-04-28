# Linux mime install script
xdg-icon-resource install --context mimetypes cockatrice.svg application/cod
xdg-icon-resource install --context mimetypes cockatrice.svg application/cor
xdg-icon-resource install --context mimetypes cockatrice.svg x-scheme-handle/cockatrice

xdg-mime install ../cockatrice-cockatrice.xml

# copy desktop file
cp -f cockatrice.desktop ~/.local/share/applications/$APP.desktop

# update databases for both application and mime
update-desktop-database ~/.local/share/applications
update-mime-database ~/.local/share/mime

# copy associated icons to pixmaps
cp cockatrice.png ~/.local/share/pixmaps
cp cockatrice.svg ~/.local/share/pixmaps
