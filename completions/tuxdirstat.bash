# bash completion for tuxdirstat, tuxdirstat-gtk3, tuxdirstat-gtk4 and tuxdirstat-tk

_tuxdirstat()
{
    local cur prev
    _init_completion || return

    case $prev in
        --export | --csv)
            _filedir
            return
            ;;
        --import | --compare)
            _filedir '@(json|gz|zst|xz|bz2)'
            return
            ;;
        --lang)
            COMPREPLY=($(compgen -W 'en tr' -- "$cur"))
            return
            ;;
        --top | --group-min | --find)
            return
            ;;
    esac

    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W '--report --find --duplicates --cleanup --owners --unowned
            --export --csv --compare --import --top --apparent --group --group-min --all-fs
            --lang --help --version' -- "$cur"))
        return
    fi
    _filedir
}
complete -F _tuxdirstat tuxdirstat tuxdirstat-gtk3 tuxdirstat-gtk4 tuxdirstat-tk
