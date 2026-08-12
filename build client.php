<?php
chdir("client");

$zip = new ZipArchive();
$zip->open("../client.zip", ZipArchive::CREATE) or die("Failed to create client.zip");
$zip->addFile("manifest.json");
$zip->addFile("content_script.js"); // generated file
$zip->addFile("background.js");
$zip->close();

$zip = new ZipArchive();
$zip->open("../client.source.zip", ZipArchive::CREATE) or die("Failed to create client.zip");
$zip->addFile("manifest.json");
$zip->addFile("make_content_script.php"); // generates content_script.js
$zip->addFile("WebHID-for-Firefox.js"); // used to generate content_script.js
$zip->addFile("background.js");
$zip->close();
