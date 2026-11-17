package org.sims2.port;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** The running game's registry supplies commands; labels never change their exact syntax. */
final class CheatCommands {
    static final class Command {
        final String name,help;
        Command(String row){int tab=row.indexOf('\t');name=tab<0?row:row.substring(0,tab);help=tab<0?"":row.substring(tab+1);}
        int group(){
            String n=name.toLowerCase(Locale.ROOT);
            if(n.contains("money")||n.contains("fund")||n.equals("kaching")||n.equals("motherlode")||n.contains("aspiration")||n.contains("motive")||n.contains("careerreward"))return 0;
            if(n.contains("aging")||n.contains("agesim")||n.contains("simdna")||n.contains("simoutfit")||n.contains("skeleton")||n.contains("headline")||n.contains("plumbbob")||n.contains("neighbortofamily")||n.equals("forcetwins")||n.equals("faceblendlimits"))return 1;
            if(n.contains("object")||n.contains("wall")||n.contains("fence")||n.contains("awning")||n.contains("roof")||n.contains("floor")||n.contains("level")||n.contains("placement"))return 2;
            if(n.contains("neighborhood")||n.contains("lot")||n.contains("terrain")||n.contains("vacation"))return 3;
            if(n.equals("sethour")||n.contains("slowmotion")||n.contains("speed")||n.contains("time")||n.startsWith("displayanim"))return 4;
            if(n.contains("bloom")||n.contains("vignette")||n.contains("filmgrain")||n.contains("letterbox")||n.contains("render")||n.contains("vsync")||n.contains("effect")||n.contains("audio")||n.equals("eh")||n.equals("aev")||n.equals("ams")||n.equals("listener")||n.equals("fog")||n.equals("overlay")||n.equals("lightmap")||n.equals("unlitroomcolor"))return 5;
            if(n.endsWith("prop")||n.equals("help")||n.equals("clear")||n.equals("exit")||n.equals("expand")||n.equals("history")||n.equals("gameversion"))return 6;
            return 7;
        }
        String title(boolean ru){
            if(!ru)return name;
            String label=russianTitle();return label==null?name:label+" — "+name;
        }
        String help(boolean ru){
            String description=ru?russianTitle():null;
            String translated=ru?russianHelp():null;
            String text=description==null?name:description+" — "+name;
            if(translated!=null)text+="\n\n"+translated;
            return help.isEmpty()?text:text+"\n\n"+(ru?"Синтаксис и справка оригинальной игры:\n":"Original game syntax and help:\n")+help;
        }
        private String russianTitle(){
            switch(name.toLowerCase(Locale.ROOT)){
                case "kaching":return "Добавить семье 1 000 §";
                case "motherlode":return "Добавить семье 50 000 §";
                case "money":return "Средства текущей семьи";
                case "familyfunds":return "Средства указанной семьи";
                case "maxmotives":return "Заполнить потребности всех симов на участке";
                case "motivedecay":return "Убывание потребностей";
                case "lockaspiration":return "Зафиксировать стремление";
                case "aspiration":return "Счёт стремления и баллы наград";
                case "aspirationpoints":return "Баллы наград за стремление";
                case "aspirationlevel":return "Уровень стремления";
                case "unlockcareerrewards":return "Открыть награды за карьеру";
                case "aging":return "Старение симов";
                case "disablepuppykittenaging":return "Отключить старение щенков и котят";
                case "agesimscheat":return "Выбор возраста сима";
                case "addneighbortofamilycheat":return "Добавление соседей в семью";
                case "forcetwins":return "Рождение близнецов";
                case "stretchskeleton":return "Рост сима";
                case "showheadlines":return "Облачка мыслей и речи";
                case "plumbbobtoggle":return "Кристалл над симом";
                case "simdna":return "ДНК сима";
                case "simoutfit":return "Одежда сима";
                case "setsimoutfitoverride":return "Переопределить костюм сима";
                case "faceblendlimits":return "Ограничения изменения лица";
                case "purgefacesystemmemory":return "Очистить память системы создания лиц";
                case "moveobjects":return "Свободное размещение предметов";
                case "setquartertileplacement":return "Размещение с шагом в четверть клетки";
                case "roofslopeangle":case "roofslopefactor":return "Угол ската крыши";
                case "individualroofslopeangle":case "individualroofslopefactor":return "Угол отдельного ската крыши";
                case "sethighestallowedlevel":return "Предел этажей";
                case "deleteallobjects":return "Удалить предметы указанного типа";
                case "deleteallwalls":return "Удалить стены";
                case "deleteallfences":return "Удалить заборы";
                case "deleteallhalfwalls":return "Удалить полустены";
                case "deleteallawnings":return "Удалить навесы";
                case "changelotzoning":return "Изменить назначение участка";
                case "changelotclassification":return "Изменить класс участка";
                case "printlotclass":return "Показать класс участка";
                case "clearlotclassvalue":return "Сбросить класс участка";
                case "modifyneighborhoodterrain":return "Изменять рельеф города";
                case "terraintype":return "Тип поверхности города";
                case "createneighborhood":return "Создать город";
                case "deleteneighborhood":return "Удалить город";
                case "installneighborhood":return "Добавить район из шаблона";
                case "changeneighborhoodtype":return "Изменить тип района";
                case "installallvacationneighborhoods":return "Добавить все туристические районы";
                case "deleteallcharacters":return "Удалить всех персонажей города";
                case "loadlot":return "Загрузить участок";
                case "sethour":return "Установить час";
                case "slowmotion":return "Замедление анимации";
                case "bugjartimedecay":return "Срок жизни насекомых в банке";
                case "displayanimlist":return "Показать список анимаций";
                case "displayanimbillboard":return "Показать активные анимации над объектами";
                case "timeofday":return "Состояние и время освещения";
                case "transitiontimes":return "Время перехода освещения";
                case "bloom":return "Свечение изображения";
                case "vignette":return "Размытие изображения";
                case "filmgrain":return "Зернистость изображения";
                case "letterbox":return "Чёрные полосы кадра";
                case "vsync":return "Вертикальная синхронизация";
                case "setlotlightingfile":return "Файл освещения участка";
                case "fog":return "Параметры тумана";
                case "unlitroomcolor":return "Цвет неосвещённой комнаты";
                case "lightmap":return "Параметры карты освещения";
                case "control":return "Управление освещением";
                case "effect":return "Управление визуальным эффектом";
                case "effectsmanager":return "Диспетчер визуальных эффектов";
                case "rttmanager":return "Диспетчер текстур рендера";
                case "overlay":return "Графическое наложение";
                case "eh":return "Обработчик звуковых событий";
                case "aev":return "Звуковые события";
                case "ams":return "Система фоновых звуков";
                case "listener":return "Слушатель пространственного звука";
                case "boolprop":return "Логическое свойство";
                case "intprop":case "uintprop":return "Беззнаковое целочисленное свойство";
                case "sintprop":return "Знаковое целочисленное свойство";
                case "floatprop":return "Дробное свойство";
                case "stringprop":return "Строковое свойство";
                case "listpropnames":return "Список имён свойств";
                case "listprops":return "Список значений свойств";
                case "propexists":return "Проверить существование свойства";
                case "help":case "h":case "?":return "Справка по командам";
                case "clear":return "Очистить консоль";
                case "hide":case "exit":return "Скрыть консоль";
                case "expand":case "x":return "Развернуть консоль";
                case "history":return "История команд";
                case "gameversion":return "Версия игры";
                case "integrationversion":return "Метка языковой сборки";
                case "quit":return "Выйти из игры";
                case "autopatch":return "Проверка обновлений оригинальной игры";
                case "allmenus":return "Показывать недоступные взаимодействия";
                case "debugsocial":return "Отладка социальных взаимодействий";
                case "dumphappy":return "Выгрузить журнал состояния сима";
                case "simchecktreefulllogging":return "Подробный журнал проверки поведения симов";
                case "dumplotstate":return "Выгрузить состояние предметов участка";
                case "dumpobjectinstances":return "Выгрузить список экземпляров предметов";
                case "dumpwants":return "Выгрузить желания симов";
                case "dumpnpccount":return "Количество служебных персонажей по типам";
                case "fba":return "Алгоритм поиска лучшего действия";
                case "textdebugging":return "Отладка отображения текста";
                case "assertresponse":return "Реакция на ошибки проверок";
                case "run":return "Выполнить читы из файла";
                case "perfwin":return "Окно анализа производительности";
                case "perflog":return "Журнал производительности среднего кадра";
                case "perflogpeaks":return "Журнал пикового времени выполнения";
                case "perflogstats":return "Журнал статистики среднего кадра";
                case "perfloglongframes":return "Журнал долгих кадров";
                case "perfcollect":return "Сбор данных производительности";
                case "perfreset":return "Сбросить данные производительности";
                case "perfscopeautodisable":return "Автоотключение частых измерений";
                case "perfscopedisable":return "Отключить именованное измерение";
                case "perfscopeenable":return "Включить именованное измерение";
                case "perfscopevtunedisable":return "Отключить измерение в VTune";
                case "perfscopevtuneenable":return "Включить измерение в VTune";
                case "metricsreset":return "Сбросить собранные метрики";
                case "metricsreport":return "Отчёт метрик в CSV";
                case "metricsreporthtml":return "Отчёт метрик в XHTML";
                case "argspectest":return "Проверка разбора аргументов";
                case "logfilter":return "Создать фильтр журнала";
                case "logformatter":return "Создать формат журнала";
                case "logfilterlevel":return "Уровень группы в фильтре журнала";
                case "logreporterlevel":return "Уровень группы для получателя журнала";
                case "logservicelevel":return "Общий уровень журналирования";
                case "logpreset":return "Создать шаблон формата журнала";
                case "logformatterpreset":return "Применить шаблон формата журнала";
                case "logreporterprop":return "Свойства получателя журнала";
                case "logfile":return "Создать файловый журнал";
                case "logwindow":return "Создать окно журнала";
                case "logsettings":return "Выгрузить настройки журналирования";
                case "pitextcvt":return "Преобразовать строки для установщика пакетов";
                case "comparedriverbuild":return "Сравнить версии видеодрайвера";
                case "include":return "Включить файл сценария";
                case "sinclude":return "Включить необязательный файл сценария";
                case "wildinclude":return "Включить файлы сценария по маске";
                case "end":return "Завершить блок сценария";
                case "ignoreerrors":return "Продолжать сценарий после ошибок";
                case "list":return "Список команд и функций парсера";
                case "eval":return "Выполнить аргумент как команду";
                case "assert":return "Проверить условие сценария";
                case "set":return "Значение переменной";
                case "sete":return "Переменная с перечислением значений";
                case "setb":return "Логическая переменная";
                case "seti":return "Целочисленная переменная";
                case "setf":return "Дробная переменная";
                case "setc":return "Переменная цвета RGB или ARGB";
                case "setv2":return "Переменная с двумя компонентами";
                case "setv3":return "Переменная с тремя компонентами";
                case "setv4":return "Переменная с четырьмя компонентами";
                case "namespace":return "Открыть пространство имён";
                case "purge":return "Удалить переменные пространства имён";
                case "if":return "Начать условный блок";
                case "elseif":return "Дополнительное условие блока";
                case "else":return "Альтернативная ветвь блока";
                case "endif":return "Завершить условный блок";
                case "define":return "Определить макрос";
                case "enddef":return "Завершить определение макроса";
                case "undefine":return "Удалить определение макроса";
                case "create":return "Создать экземпляр определения";
                case "arraycreate":return "Создать массив экземпляров определения";
                case "trace":return "Вывести строку сценария";
                case "log":return "Вывести сообщение журнала";
                case "dump":return "Выгрузить состояние парсера";
                case "debug":return "Средства отладки парсера";
                case "showarguments":return "Показать аргументы команды";
                case "debuggerbreak":return "Остановиться в отладчике";
                case "version":return "Версия текущего сценария";
                case "alias":return "Псевдоним команды";
                default:return null;
            }
        }
        private String russianHelp(){
            switch(name.toLowerCase(Locale.ROOT)){
                case "money":return "Число задаёт сумму средств; знак + или - перед числом добавляет или вычитает указанную сумму.";
                case "familyfunds":return "Укажите имя семьи и сумму. Знак + или - перед суммой позволяет добавить или вычесть средства.";
                case "maxmotives":return "Заполняет все потребности всех симов на текущем участке.";
                case "motivedecay":return "on — потребности убывают; off — убывание отключено. Действует на всех симов участка.";
                case "lockaspiration":return "on — зафиксировать счёт стремления симов участка; off — снять фиксацию.";
                case "aspirationpoints":return "Укажите число добавляемых баллов наград для выбранного сима.";
                case "aspirationlevel":return "Укажите уровень стремления выбранного сима от 0 до 5.";
                case "sethour":return "Укажите час суток от 0 до 23.";
                case "slowmotion":return "Укажите степень замедления от 0 до 8. Значение 0 возвращает обычную скорость анимации.";
                case "stretchskeleton":return "Множитель 1.0 сохраняет обычный рост; меньше 1.0 уменьшает рост, больше 1.0 увеличивает.";
                case "aging":return "on — включить старение; off — отключить.";
                case "disablepuppykittenaging":return "on — запретить старение щенков и котят; off — вернуть обычное старение.";
                case "showheadlines":return "on — показывать облачка мыслей и речи; off — скрыть.";
                case "moveobjects":return "on разрешает перемещение обычно неподвижных предметов; off возвращает обычные ограничения.";
                case "roofslopeangle":case "individualroofslopeangle":return "Угол в градусах от 15 до 75; обычное значение — 45.";
                case "deleteallobjects":return "Укажите doors, windows или stairs для удаления дверей, окон или лестниц.";
                case "deleteallcharacters":return "Удаляет всех персонажей текущего города. Команда работает только на экране города.";
                case "createneighborhood":return "Укажите имя города, путь к файлу рельефа SC4 и при необходимости тип. Игра создаст город и переключится на него.";
                case "deleteneighborhood":return "Укажите имя удаляемого города.";
                case "installneighborhood":return "Укажите имя шаблона и имя нового района; район добавится к текущему основному городу.";
                case "loadlot":return "Укажите имя участка. Необязательное имя семьи позволяет также заселить её на участок.";
                case "modifyneighborhoodterrain":return "on/off включает или отключает изменение рельефа города. Выберите область курсором: p — сгладить, [ — поднять, ] — опустить, \\ — выровнять.";
                case "terraintype":return "Temperate — трава; Desert — пустыня; Dirt — земля; Concrete — бетон.";
                case "bloom":return "R, G и B задают цвет, X — силу свечения; каждое значение от 0 до 1. Сначала выполните boolProp enablePostProcessing true.";
                case "vignette":return "centerX и centerY задают центр размытия: 0,0 — верхний левый угол, 1,1 — нижний правый. X от 0 до 1 задаёт силу. Сначала выполните boolProp enablePostProcessing true.";
                case "filmgrain":return "Значение зернистости от 0 до 1. Сначала выполните boolProp enablePostProcessing true.";
                case "letterbox":return "Толщина полос сверху и снизу — от 0 до 0.4. Сначала выполните boolProp enablePostProcessing true.";
                case "allmenus":return "on/off показывает или скрывает недоступные действия в меню персонажа.";
                case "debugsocial":return "on/off включает или отключает диалоги выбора результата социальных взаимодействий.";
                case "fba":return "1 — исходный алгоритм поиска лучшего действия; 2 — экспериментальный.";
                case "run":return "Укажите абсолютный путь к файлу. Оригинальный парсер выполнит каждую его строку как чит.";
                case "boolprop":return "Укажите имя свойства и true или false. Имена свойств сохраняются на языке оригинала.";
                case "intprop":case "uintprop":return "Укажите имя свойства и беззнаковое целое значение.";
                case "sintprop":return "Укажите имя свойства и целое значение со знаком.";
                case "floatprop":return "Укажите имя свойства и дробное значение с точкой.";
                case "stringprop":return "Укажите имя свойства и строковое значение.";
                default:return null;
            }
        }
    }
    static List<Command> parse(String[] rows){
        List<Command> commands=new ArrayList<>();
        java.util.Set<String> seen=new java.util.HashSet<>();
        if(rows!=null)for(String row:rows)if(row!=null&&!row.isEmpty()&&seen.add(row))commands.add(new Command(row));
        commands.sort((a,b)->a.name.compareToIgnoreCase(b.name));return commands;
    }
    static String[] groups(boolean ru){return ru?new String[]{"Деньги, потребности и награды","Симы и семья","Строительство и предметы","Город и участки","Время и анимация","Изображение, эффекты и звук","Свойства и консоль","Диагностика и другие команды"}:new String[]{"Money, needs and rewards","Sims and family","Building and objects","Neighborhood and lots","Time and animation","Graphics, effects and audio","Properties and console","Diagnostics and other commands"};}
}
