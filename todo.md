# cat *.mp4
 . This is for future but we should come up with some clever way and high performance solution to add sound to video playback


# Pipes
 . make builtin commands work with pipes with outer tools, this really makes me thinking because outside grep does not see our command output, but if we implement this, while implementing it we should not by anu means compromise performance, or accept any drowback that ruins already established comfortable experience


# Explorer.h
    . Long names should hide if no space on tab, now they are hiding but with unicode chars in filename there could be some edge case?
    Rename: I delete path in move i.e. not full path it means I am naming new name?
        Should work on folder and on file

    . Hardest main challange of explorer is whyle CTRL+O toggle brings the comfort it was meant to, it ruins original terminal colors when switched back, the speed of toggle and experience is important but we should come up with out of the box solution to keep same vivid colors back in the terminal, the problem I do not understand is that the original terminal runns in different buffer and the Explorer in different, and switching back changes colors of the Original terminal buffer of what idk, and the Explorer has also some dimmed collors, one solution I propose is based on my visual intel, when we go from explorer to terminal colors are dimmed (I mean what was already drawn in terminal those change colors, for example some lime or green becomes cyan and so on) but if I run some new command in terminal it draws in original vivid colors it was meant to, which means new output is not ruined, only old, which was already there, so if our primary goal is to preserve colors in the terminal which was already there, can we just copy terminal buffer and we go back to terminal from explorer redraw it fully or something? like redraw screen after going back, it will be one flush, and user will not notice it

# Top
    In top filter do same cursor system as in explorer filter

# Edit
    Display edit dialog like in file explorer with buttons?

# Resmon
    I have seen cool resmon in reddit which instead of progressbars displays short history graph of resource, like in our mp3 player.h audio visualiser where we display waves of audio in short window, we can aggregate short history of progressbar values and display them scrolling to the left

