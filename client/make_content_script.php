<?php
$script = file_get_contents("WebHID-for-Firefox.js");
$script = str_replace('`', '\`', $script);
$script = str_replace('$', '\$', $script);
file_put_contents("content_script.js", "const script=document.createElement(\"script\");script.innerHTML=`".$script."`;document.documentElement.appendChild(script);");
