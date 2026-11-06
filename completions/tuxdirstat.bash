# bash completion for tuxdirstat, tuxdirstat-gtk3, tuxdirstat-gtk4, tuxdirstat-tk and tuxdirstat-tui

_tuxdirstat()
{
    local cur prev
    _init_completion || return

    case $prev in
        --export | --csv | --html | --png | --sunburst)
            _filedir
            return
            ;;
        --import | --compare)
            _filedir '@(json|gz|zst|xz|bz2)'
            return
            ;;
        --lang)
            COMPREPLY=($(compgen -W 'en tr' -- "$cur"))  # po/*.po
            return
            ;;
        --top | --group-min | --find | --size)
            return
            ;;
    esac

    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W '--report --find --duplicates --cleanup --owners --unowned
            --export --csv --compare --html --png --sunburst --btrfs --history --packages
            --history-record --import --top --size --apparent --group --group-min --all-fs
            --lang --help --version' -- "$cur"))
        return
    fi
    _filedir
}
complete -F _tuxdirstat tuxdirstat tuxdirstat-gtk3 tuxdirstat-gtk4 tuxdirstat-tk tuxdirstat-tui
