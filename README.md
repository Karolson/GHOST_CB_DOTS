# DodsGHost

## Генерация проекта Visual Studio

1. [Ищем здесь .msi установщик Meson и устанавливаем его](https://github.com/mesonbuild/meson/releases)
2. Находим в пуске папку с Visual Studio, там должен быть файл с названием вроде `x64 Native Tools Command Prompt for VS 2019`, запускаем его
3. Через `cd /D путь` переходим в папку куда скачали dodsghost. Важно чтобы в пути не было пробелов
4. Пишем `meson --backend vs --buildtype debug builddir`
5. В случае успеха в папке `builddir` появится проект для Visual Studio

В Visual Studio должен быть установлен компонент `Windows SDK`

## Альтернативно

Имеется нативная сборка для Линукса и сборка для Шиндовса через MinGW
