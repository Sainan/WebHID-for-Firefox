<?php
file_put_contents("content_script.js", "const script=document.createElement(\"script\");script.innerHTML=`".file_get_contents("WebHID-for-Firefox.js")."`;document.documentElement.appendChild(script);");
